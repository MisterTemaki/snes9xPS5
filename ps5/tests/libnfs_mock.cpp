// An in-memory stand-in for libnfs: see libnfs_mock.h.
//
// SPDX-License-Identifier: MIT
#include "libnfs_mock.h"

#include <poll.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <set>

#include <nfsc/libnfs.h>
#include <nfsc/libnfs-raw.h>

#include "libnfs-raw-mount.h"

struct nfs_context
{
	std::string error;
	std::string rpc_error;
	std::string root;
	int version = 3;
	size_t readmax = 1u << 20;
	bool dead = false;
	std::deque<std::function<void(bool cancel)>> queue; // completions, run one per nfs_service
	std::set<nfsfh*> open;                              // libnfs leaves these to the caller; the mock counts and frees them
};

struct nfsfh
{
	std::string path;
};

struct nfsdir
{
	std::vector<std::string> names;
	std::vector<nfsdirent> ents;
	size_t pos = 0;
};

namespace mocknfs
{
Server& S()
{
	static Server s;
	return s;
}

void Reset()
{
	S() = Server();
}

void AddDir(const std::string& path)
{
	Node n;
	n.dir = true;
	S().nodes[path] = n;
}

void AddFile(const std::string& path, const std::string& data, int64_t mtime)
{
	Node n;
	n.data = data;
	n.mtime = mtime;
	S().nodes[path] = n;
}
} // namespace mocknfs

namespace
{
using mocknfs::Node;
using mocknfs::S;

void Touch(nfs_context* c)
{
	if (c->dead)
		S().calls_after_death++;
}

std::string Abs(const nfs_context* c, const std::string& rel)
{
	if (rel == "/")
		return c->root;
	return (c->root == "/" ? std::string() : c->root) + rel;
}

// follows one symlink; nullptr when the path does not exist
const Node* Find(const std::string& abs)
{
	auto it = S().nodes.find(abs);
	if (it == S().nodes.end())
		return nullptr;
	if (!it->second.link.empty())
	{
		auto t = S().nodes.find(it->second.link);
		return t == S().nodes.end() ? nullptr : &t->second;
	}
	return &it->second;
}

// mountd accepts a path that is an export or inside one (every existing path when the server lists none)
bool Accepts(const std::string& path)
{
	if (!S().accepts.empty())
		return S().accepts.count(path) != 0;
	if (S().exports.empty())
		return true;
	for (const std::string& e : S().exports)
		if (path == e || (path.size() > e.size() && path.compare(0, e.size(), e) == 0 && path[e.size()] == '/'))
			return true;
	return false;
}

void Enqueue(nfs_context* c, std::function<void(bool)> f)
{
	if (!S().hang)
		c->queue.push_back(std::move(f));
	else
		c->queue.push_back([f](bool cancel) {
			if (cancel)
				f(true);
		});
}

// completes later, from nfs_service; `data` is built when it runs
void Complete(nfs_context* c, nfs_cb cb, void* priv, int status, void* data = nullptr)
{
	Enqueue(c, [=](bool cancel) { cb(cancel ? -ECANCELED : status, c, cancel ? nullptr : data, priv); });
}
} // namespace

extern "C" {
nfs_context* nfs_init_context(void)
{
	return new nfs_context;
}

void nfs_destroy_context(nfs_context* c)
{
	// libnfs may still run the callbacks of calls it never completed
	auto q = std::move(c->queue);
	for (auto& f : q)
		f(true);
	for (nfsfh* fh : c->open)
	{
		S().leaked_handles++; // real libnfs would leak it; the mock frees it for the sanitizers and counts it
		delete fh;
	}
	delete c;
}

void nfs_set_readmax(nfs_context* c, size_t n) { c->readmax = n; }
size_t nfs_get_readmax(nfs_context* c) { return c->readmax; }

int nfs_set_version(nfs_context* c, int v)
{
	c->version = v;
	S().version_set = v;
	return 0;
}
void nfs_set_autoreconnect(nfs_context*, int) {}
void nfs_set_dircache(nfs_context*, int) {}
void nfs_set_nfsport(nfs_context*, int p) { S().nfsport_set = p; }
void nfs_set_mountport(nfs_context*, int p) { S().mountport_set = p; }
void nfs_set_uid(nfs_context*, int u) { S().uid_set = u; }
void nfs_set_gid(nfs_context*, int g) { S().gid_set = g; }
char* nfs_get_error(nfs_context* c) { return const_cast<char*>(c->error.c_str()); }
// the mock keeps the RPC layer's error string on the same object
rpc_context* nfs_get_rpc_context(nfs_context* c) { return reinterpret_cast<rpc_context*>(c); }
char* rpc_get_error(rpc_context* r) { return const_cast<char*>(reinterpret_cast<nfs_context*>(r)->rpc_error.c_str()); }
int nfs_get_fd(nfs_context*) { return -1; }
int nfs_which_events(nfs_context*) { return POLLIN; }

int nfs_service(nfs_context* c, int)
{
	S().services++;
	if (S().on_service)
		S().on_service();
	if (c->dead || S().services == S().fail_service_at)
	{
		c->dead = true;
		c->error = "Socket failed with POLLHUP";
		return -1;
	}
	if (c->queue.empty())
		return 0;
	auto f = std::move(c->queue.front());
	c->queue.pop_front();
	f(false);
	return 0;
}

int nfs_mount_async(nfs_context* c, const char* server, const char* exp, nfs_cb cb, void* priv)
{
	Touch(c);
	S().mount_host = server;
	S().mount_paths.push_back(exp);
	const std::string path = exp;
	int status = 0;
	if (S().refuse)
	{
		c->error = "connect() to server failed. Connection refused(111)";
		status = -EFAULT;
	}
	else
	{
		const Node* n = Find(path);
		if (!n || !n->dir)
		{
			c->error = "MOUNT failed with MNT3ERR_NOENT";
			status = -ENOENT;
		}
		else if (S().exports_denied || !Accepts(path))
		{
			c->error = "MOUNT failed with MNT3ERR_ACCES";
			status = -EACCES;
		}
		else
		{
			c->root = path;
			c->readmax = std::min(c->readmax, S().rtmax); // the server's FSINFO
		}
	}
	Complete(c, cb, priv, status);
	return 0;
}

int nfs_stat64_async(nfs_context* c, const char* path, nfs_cb cb, void* priv)
{
	Touch(c);
	const Node* n = Find(Abs(c, path));
	if (!n)
	{
		Complete(c, cb, priv, -ENOENT);
		return 0;
	}
	Enqueue(c, [=](bool cancel) {
		nfs_stat_64 st = {};
		st.nfs_mode = (n->dir ? S_IFDIR : S_IFREG) | 0644;
		st.nfs_size = n->data.size();
		st.nfs_mtime = static_cast<uint64_t>(n->mtime);
		cb(cancel ? -ECANCELED : 0, c, &st, priv);
	});
	return 0;
}

int nfs_opendir_async(nfs_context* c, const char* path, nfs_cb cb, void* priv)
{
	Touch(c);
	const std::string abs = Abs(c, path);
	const Node* n = Find(abs);
	if (!n)
	{
		Complete(c, cb, priv, -ENOENT);
		return 0;
	}
	if (!n->dir)
	{
		Complete(c, cb, priv, -ENOTDIR);
		return 0;
	}
	auto* d = new nfsdir;
	d->names = {".", ".."};
	const std::string prefix = abs == "/" ? "/" : abs + "/";
	for (const auto& kv : S().nodes)
		if (kv.first.size() > prefix.size() && kv.first.compare(0, prefix.size(), prefix) == 0 &&
		    kv.first.find('/', prefix.size()) == std::string::npos)
			d->names.push_back(kv.first.substr(prefix.size()));
	d->ents.resize(d->names.size());
	for (size_t i = 0; i < d->names.size(); i++)
	{
		nfsdirent& e = d->ents[i];
		e.name = const_cast<char*>(d->names[i].c_str());
		auto it = S().nodes.find(prefix + d->names[i]);
		if (it == S().nodes.end())
		{
			e.mode = S_IFDIR | 0755; // "." and ".."
			continue;
		}
		const Node& m = it->second;
		if (m.no_attrs)
			continue;
		e.mode = (!m.link.empty() ? S_IFLNK : m.dir ? S_IFDIR : S_IFREG) | 0644;
		e.size = m.data.size();
		e.mtime.tv_sec = m.mtime;
	}
	Enqueue(c, [=](bool cancel) {
		if (cancel)
		{
			delete d;
			cb(-ECANCELED, c, nullptr, priv);
		}
		else
			cb(0, c, d, priv);
	});
	return 0;
}

nfsdirent* nfs_readdir(nfs_context*, nfsdir* d)
{
	return d->pos < d->ents.size() ? &d->ents[d->pos++] : nullptr;
}

void nfs_closedir(nfs_context*, nfsdir* d)
{
	delete d;
}

int nfs_open_async(nfs_context* c, const char* path, int, nfs_cb cb, void* priv)
{
	Touch(c);
	const std::string abs = Abs(c, path);
	const Node* n = Find(abs);
	if (!n)
	{
		Complete(c, cb, priv, -ENOENT);
		return 0;
	}
	auto* fh = new nfsfh;
	fh->path = abs;
	c->open.insert(fh);
	Enqueue(c, [=](bool cancel) {
		if (cancel)
			cb(-ECANCELED, c, nullptr, priv);
		else
			cb(0, c, fh, priv);
	});
	return 0;
}

int nfs_pread_async(nfs_context* c, nfsfh* fh, void* buf, size_t count, uint64_t offset, nfs_cb cb, void* priv)
{
	Touch(c);
	count = std::min(count, c->readmax); // like libnfs: one request, never more than the negotiated size
	S().last_read_len = static_cast<int>(count);
	S().max_read_len = std::max(S().max_read_len, static_cast<int>(count));
	S().in_flight++;
	S().max_in_flight = std::max(S().max_in_flight, S().in_flight);
	Enqueue(c, [=](bool cancel) {
		S().in_flight--;
		if (cancel)
		{
			cb(-ECANCELED, c, nullptr, priv);
			return;
		}
		const Node* n = Find(fh->path);
		size_t got = 0;
		if (n && offset < n->data.size())
		{
			size_t want = count;
			if (S().short_read > 0 && want > static_cast<size_t>(S().short_read))
				want = static_cast<size_t>(S().short_read);
			got = std::min(want, n->data.size() - static_cast<size_t>(offset));
			std::memcpy(buf, n->data.data() + offset, got);
		}
		cb(static_cast<int>(got), c, buf, priv);
	});
	return 0;
}

int nfs_close_async(nfs_context* c, nfsfh* fh, nfs_cb cb, void* priv)
{
	if (c->version != 4)
	{
		// NFSv3, file never written: libnfs frees the handle locally and calls back at once, with no I/O
		c->open.erase(fh);
		delete fh;
		cb(0, c, nullptr, priv);
		return 0;
	}
	Touch(c);
	Enqueue(c, [=](bool cancel) {
		if (!cancel)
		{
			c->open.erase(fh);
			delete fh;
		}
		cb(cancel ? -ECANCELED : 0, c, nullptr, priv);
	});
	return 0;
}

exportnode* mount_getexports_timeout(const char*, int)
{
	exportnode* head = nullptr;
	if (S().refuse)
		return nullptr;
	for (auto it = S().exports.rbegin(); it != S().exports.rend(); ++it)
	{
		auto* e = static_cast<exportnode*>(std::calloc(1, sizeof(exportnode)));
		e->ex_dir = strdup(it->c_str());
		e->ex_next = head;
		head = e;
	}
	return head;
}

void mount_free_export_list(exportnode* e)
{
	while (e)
	{
		exportnode* next = e->ex_next;
		std::free(e->ex_dir);
		std::free(e);
		e = next;
	}
}
} // extern "C"
