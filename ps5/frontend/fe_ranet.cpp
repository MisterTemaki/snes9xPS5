// Snes9x PS5 (2.11): the app's RetroAchievements requests through the helper (fe_ranet.h).
// SPDX-License-Identifier: MIT

#include "fe_ranet.h"

#include "OrbisPaths.h"
#include "ProsperoJailbreak.h"
#include "ProsperoThread.h"

#include <atomic>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <sys/socket.h>

namespace ra::net
{
namespace
{
struct Job
{
	std::string url, post, agent, what;
	Done done;
	Answer answer;
};

std::mutex g_mu;
std::condition_variable g_cv;
std::deque<std::unique_ptr<Job>> g_todo, g_done;
std::unique_ptr<ps5::BigThread> g_thread;
bool g_stop = false;
std::atomic<int> g_socket{-1};
std::atomic<int> g_in_flight{0};

double Now()
{
	timespec ts = {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

void Worker()
{
	for (;;)
	{
		std::unique_ptr<Job> job;
		{
			std::unique_lock<std::mutex> lock(g_mu);
			g_cv.wait(lock, [] { return g_stop || !g_todo.empty(); });
			if (g_stop)
				return;
			job = std::move(g_todo.front());
			g_todo.pop_front();
		}
		const double t0 = Now();
		const jailbreak::RaReply r = jailbreak::RaRequest(job->url, job->post, job->agent, &g_socket);
		// the body held the password or the token: wiped before it is let go
		for (char& c : job->post)
			*(volatile char*)&c = 0;
		Answer& a = job->answer;
		a.why = r.why;
		switch (r.call)
		{
			case jailbreak::RaCall::Answer:
				a.outcome = Outcome::Http;
				a.http = r.http;
				a.body = r.body;
				break;
			case jailbreak::RaCall::NoHelper: a.outcome = Outcome::NoHelper; break;
			case jailbreak::RaCall::OldHelper: a.outcome = Outcome::OldHelper; break;
			case jailbreak::RaCall::HelperError:
				a.outcome = r.helper_ret == jailbreak::kRaNetwork ? Outcome::Offline : Outcome::Refused;
				break;
		}
		static const char* const kNames[] = {"answered", "no helper", "an older helper", "no server", "refused"};
		OrbisLog("[ra] %s: %s %d, %zu bytes, %.0f ms%s%s", job->what.c_str(), kNames[int(a.outcome)], a.http, a.body.size(),
			(Now() - t0) * 1000.0, a.why.empty() ? "" : ": ", a.why.c_str());
		std::lock_guard<std::mutex> lock(g_mu);
		if (g_stop)
			return;
		g_done.push_back(std::move(job));
	}
}
} // namespace

void Send(const std::string& url, const std::string& post, const std::string& user_agent, const std::string& what,
	Done done)
{
	std::unique_ptr<Job> job(new Job);
	job->url = url;
	job->post = post;
	job->agent = user_agent;
	job->what = what;
	job->done = std::move(done);
	std::unique_lock<std::mutex> lock(g_mu);
	g_stop = false;
	if (!g_thread)
	{
		g_thread.reset(new ps5::BigThread(Worker, 256 * 1024));
		if (!g_thread->joinable())
		{
			// no thread to be had: the request fails as if the network were down (rc_client tries again later)
			g_thread.reset();
			lock.unlock();
			OrbisLog("[ra] no thread for the requests: %s not sent", what.c_str());
			job->answer.outcome = Outcome::Offline;
			job->answer.why = "no thread";
			std::lock_guard<std::mutex> again(g_mu);
			g_done.push_back(std::move(job));
			g_in_flight++;
			return;
		}
	}
	g_todo.push_back(std::move(job));
	g_in_flight++;
	g_cv.notify_one();
}

void Pump()
{
	std::deque<std::unique_ptr<Job>> ready;
	{
		std::lock_guard<std::mutex> lock(g_mu);
		ready.swap(g_done);
	}
	for (std::unique_ptr<Job>& job : ready)
	{
		g_in_flight--;
		if (job->done)
			job->done(job->answer);
	}
}

int InFlight()
{
	return g_in_flight.load();
}

void Stop()
{
	{
		std::lock_guard<std::mutex> lock(g_mu);
		g_stop = true;
		g_todo.clear();
		g_done.clear();
	}
	g_cv.notify_all();
	const int fd = g_socket.load();
	if (fd >= 0)
		shutdown(fd, SHUT_RDWR); // the request under way ends now, not after the helper's timeout
	g_thread.reset(); // joins
	g_in_flight = 0;
}
} // namespace ra::net
