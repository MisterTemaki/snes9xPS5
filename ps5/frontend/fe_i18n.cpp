// Snes9x PS5 frontend: the interface's languages (fe_i18n.h).
// SPDX-License-Identifier: MIT

#include "fe_i18n.h"

#include "fe_settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

namespace fe
{
namespace
{
// English, Spanish, Portuguese (Brazil), French, German. A text missing here is shown in English.
struct Entry
{
	const char* en;
	const char* es;
	const char* pt;
	const char* fr;
	const char* de;
};

const Entry kTable[] = {
	// ---- the settings
	{"On", "Sí", "Sim", "Oui", "Ein"},
	{"Off", "No", "Não", "Non", "Aus"},
	{"Shader", "Shader", "Shader", "Shader", "Shader"},
	{"Aspect ratio", "Relación de aspecto", "Proporção da tela", "Format d'image", "Seitenverhältnis"},
	{"Smooth picture (shader Off)", "Imagen suavizada (shader desactivado)", "Imagem suavizada (shader desligado)",
		"Image lissée (shader désactivé)", "Geglättetes Bild (Shader aus)"},
	{"Scanlines (shader Off)", "Scanlines (shader desactivado)", "Scanlines (shader desligado)",
		"Lignes de balayage (shader désactivé)", "Scanlines (Shader aus)"},
	{"Show FPS", "Mostrar FPS", "Mostrar FPS", "Afficher les FPS", "FPS anzeigen"},
	{"Sound", "Sonido", "Som", "Son", "Ton"},
	{"Transparency", "Transparencia", "Transparência", "Transparence", "Transparenz"},
	{"Super FX clock", "Reloj del Super FX", "Clock do Super FX", "Horloge Super FX", "Super-FX-Takt"},
	{"Download covers", "Descargar portadas", "Baixar capas", "Télécharger les jaquettes", "Cover herunterladen"},
	{"Language", "Idioma", "Idioma", "Langue", "Sprache"},
	{"Debug logs", "Registros de depuración", "Registros de depuração", "Journaux de débogage", "Debug-Protokolle"},
	{"Updates (this is %s)", "Actualizaciones (esta es la %s)", "Atualizações (esta é a %s)",
		"Mises à jour (version actuelle %s)", "Updates (diese ist %s)"},
	{"Needs the helper", "Necesita el helper", "Precisa do helper", "Nécessite le helper", "Braucht den Helper"},
	{"Checking...", "Comprobando...", "Verificando...", "Vérification...", "Prüfe..."},
	{"Up to date", "Al día", "Atualizado", "À jour", "Aktuell"},
	{"Install %s (%.1f MB)", "Instalar %s (%.1f MB)", "Instalar %s (%.1f MB)", "Installer %s (%.1f Mo)",
		"%s installieren (%.1f MB)"},
	{"Downloading %d%%", "Descargando %d%%", "Baixando %d%%", "Téléchargement %d%%", "Lade herunter %d%%"},
	{"Installing %d/%d", "Instalando %d/%d", "Instalando %d/%d", "Installation %d/%d", "Installiere %d/%d"},
	{"Installed: restart", "Instalada: reinicia", "Instalada: reinicie", "Installée : redémarrez",
		"Installiert: neu starten"},
	{"Failed: try again", "Falló: inténtalo de nuevo", "Falhou: tente de novo", "Échec : réessayez",
		"Fehlgeschlagen: erneut versuchen"},
	{"Check now", "Comprobar ahora", "Verificar agora", "Vérifier maintenant", "Jetzt prüfen"},
	{"Updates are downloaded by the helper: send the payload, then open Settings again.",
		"El helper descarga las actualizaciones: envía el payload y vuelve a abrir Ajustes.",
		"O helper baixa as atualizações: envie o payload e abra as Configurações de novo.",
		"Le helper télécharge les mises à jour : envoyez le payload, puis rouvrez les Paramètres.",
		"Updates lädt der Helper: Payload senden, dann die Einstellungen erneut öffnen."},
	{"Controller shortcuts", "Atajos del mando", "Atalhos do controle", "Raccourcis de la manette", "Controller-Kürzel"},
	{"Repair missing covers", "Reparar las portadas que faltan", "Reparar capas ausentes",
		"Réparer les jaquettes manquantes", "Fehlende Cover reparieren"},
	{"Back", "Volver", "Voltar", "Retour", "Zurück"},
	{"Settings", "Ajustes", "Configurações", "Paramètres", "Einstellungen"},
	{"Toggle", "Cambiar", "Alternar", "Basculer", "Umschalten"},
	{"Adjust", "Ajustar", "Ajustar", "Régler", "Anpassen"},
	// ---- the settings' choices
	{"CRT Easymode style", "Estilo CRT Easymode", "Estilo CRT Easymode", "Style CRT Easymode", "CRT-Easymode-Stil"},
	{"ScaleFX + rAA + AA style", "Estilo ScaleFX + rAA + AA", "Estilo ScaleFX + rAA + AA", "Style ScaleFX + rAA + AA",
		"ScaleFX + rAA + AA-Stil"},
	{"8:7 (pixel perfect)", "8:7 (píxel perfecto)", "8:7 (pixel perfeito)", "8:7 (pixel parfait)", "8:7 (pixelgenau)"},
	{"Integer scale", "Escala entera", "Escala inteira", "Échelle entière", "Ganzzahlige Skalierung"},
	{"Stretch 16:9", "Estirar a 16:9", "Esticar para 16:9", "Étirer en 16:9", "Auf 16:9 strecken"},
	{"Touchpad + Options", "Panel táctil + Options", "Touchpad + Options", "Pavé tactile + Options", "Touchpad + Options"},
	{"L2 + Triangle", "L2 + Triángulo", "L2 + Triângulo", "L2 + Triangle", "L2 + Dreieck"},
	{"R2 + Triangle", "R2 + Triángulo", "R2 + Triângulo", "R2 + Triangle", "R2 + Dreieck"},
	{"Options + Triangle", "Options + Triángulo", "Options + Triângulo", "Options + Triangle", "Options + Dreieck"},
	{"Touchpad + Triangle", "Panel táctil + Triángulo", "Touchpad + Triângulo", "Pavé tactile + Triangle",
		"Touchpad + Dreieck"},
	{"R3 + Options", "R3 + Options", "R3 + Options", "R3 + Options", "R3 + Options"},
	// ---- the folder browser
	{"Choose where your games are", "Elige dónde están tus juegos", "Escolha onde estão seus jogos",
		"Choisissez où sont vos jeux", "Wähle, wo deine Spiele sind"},
	{"Console", "Consola", "Console", "Console", "Konsole"},
	{"No games found here.", "No se encontraron juegos aquí.", "Nenhum jogo encontrado aqui.", "Aucun jeu trouvé ici.",
		"Hier wurden keine Spiele gefunden."},
	{"Copy your SNES ROMs (.sfc .smc .swc .fig .bs .zip) to:", "Copia tus ROMs de SNES (.sfc .smc .swc .fig .bs .zip) en:",
		"Copie suas ROMs de SNES (.sfc .smc .swc .fig .bs .zip) para:",
		"Copiez vos ROM SNES (.sfc .smc .swc .fig .bs .zip) dans :",
		"Kopiere deine SNES-ROMs (.sfc .smc .swc .fig .bs .zip) nach:"},
	{"or, on a USB drive, to the folder  snes9x/roms", "o, en un USB, a la carpeta  snes9x/roms",
		"ou, num pendrive, para a pasta  snes9x/roms", "ou, sur une clé USB, dans le dossier  snes9x/roms",
		"oder auf einem USB-Stick in den Ordner  snes9x/roms"},
	{"Open", "Abrir", "Abrir", "Ouvrir", "Öffnen"},
	{"Page", "Página", "Página", "Page", "Seite"},
	// ---- the pause menu
	{"Resume", "Continuar", "Continuar", "Reprendre", "Fortsetzen"},
	{"Save state (slot %d)", "Guardar estado (ranura %d)", "Salvar estado (slot %d)", "Sauvegarder l'état (emplacement %d)",
		"Spielstand speichern (Platz %d)"},
	{"Load state (slot %d)", "Cargar estado (ranura %d)", "Carregar estado (slot %d)", "Charger l'état (emplacement %d)",
		"Spielstand laden (Platz %d)"},
	{"State slot", "Ranura de estado", "Slot do estado", "Emplacement", "Spielstand-Platz"},
	{"%d (used)", "%d (usada)", "%d (usado)", "%d (utilisé)", "%d (belegt)"},
	{"%d (empty)", "%d (vacía)", "%d (vazio)", "%d (vide)", "%d (leer)"},
	{"Cheat manager", "Gestor de trucos", "Gerenciador de trapaças", "Gestionnaire de codes", "Cheat-Verwaltung"},
	{"Reset game", "Reiniciar el juego", "Reiniciar o jogo", "Réinitialiser le jeu", "Spiel zurücksetzen"},
	{"Back to the game list", "Volver a la lista de juegos", "Voltar à lista de jogos", "Retour à la liste des jeux",
		"Zurück zur Spieleliste"},
	{"Quit Snes9x", "Salir de Snes9x", "Sair do Snes9x", "Quitter Snes9x", "Snes9x beenden"},
	{"State saved.", "Estado guardado.", "Estado salvo.", "État sauvegardé.", "Spielstand gespeichert."},
	{"Could not save the state.", "No se pudo guardar el estado.", "Não foi possível salvar o estado.",
		"Impossible de sauvegarder l'état.", "Spielstand konnte nicht gespeichert werden."},
	{"This slot is empty.", "Esta ranura está vacía.", "Este slot está vazio.", "Cet emplacement est vide.",
		"Dieser Platz ist leer."},
	{"OK", "Aceptar", "OK", "OK", "OK"},
	{"Yes", "Sí", "Sim", "Oui", "Ja"},
	{"No", "No", "Não", "Non", "Nein"},
	// ---- the cheats
	{"ON", "SÍ", "SIM", "OUI", "EIN"},
	{"OFF", "NO", "NÃO", "NON", "AUS"},
	{"Cheats", "Trucos", "Trapaças", "Codes", "Cheats"},
	{"Cheats %d-%d / %d", "Trucos %d-%d / %d", "Trapaças %d-%d / %d", "Codes %d-%d / %d", "Cheats %d-%d / %d"},
	{"No cheats found", "No se encontraron trucos", "Nenhuma trapaça encontrada", "Aucun code trouvé",
		"Keine Cheats gefunden"},
	{"Place a matching .cht file in /data/snes9x/cheats", "Pon un .cht que coincida en /data/snes9x/cheats",
		"Coloque um .cht correspondente em /data/snes9x/cheats",
		"Mettez un .cht correspondant dans /data/snes9x/cheats",
		"Lege eine passende .cht-Datei in /data/snes9x/cheats"},
	{"Copy .cht file over FTP, it loads by itself     Circle: Back",
		"Copia el archivo .cht por FTP, se carga solo     Círculo: Volver",
		"Copie o arquivo .cht por FTP, ele carrega sozinho     Bola: Voltar",
		"Copiez le fichier .cht par FTP, il se charge tout seul     Rond : Retour",
		".cht-Datei per FTP kopieren, sie lädt von selbst     Kreis: Zurück"},
	{"Cheats updated: %d", "Trucos actualizados: %d", "Trapaças atualizadas: %d", "Codes mis à jour : %d",
		"Cheats aktualisiert: %d"},
	{"Cross: Toggle   L1/R1: Page   Triangle: All ON   Square: All OFF   Circle: Back",
		"Cruz: Cambiar   L1/R1: Página   Triángulo: Todos SÍ   Cuadrado: Todos NO   Círculo: Volver",
		"X: Alternar   L1/R1: Página   Triângulo: Todas SIM   Quadrado: Todas NÃO   Bola: Voltar",
		"Croix : Basculer   L1/R1 : Page   Triangle : Tous OUI   Carré : Tous NON   Rond : Retour",
		"Kreuz: Umschalten   L1/R1: Seite   Dreieck: Alle EIN   Quadrat: Alle AUS   Kreis: Zurück"},
	{"No cheat file checked", "Ningún archivo de trucos revisado", "Nenhum arquivo de trapaças verificado",
		"Aucun fichier de codes vérifié", "Keine Cheat-Datei geprüft"},
	{"Unable to open cheat file", "No se pudo abrir el archivo de trucos", "Não foi possível abrir o arquivo de trapaças",
		"Impossible d'ouvrir le fichier de codes", "Cheat-Datei konnte nicht geöffnet werden"},
	{"Found %s but no supported codes", "Se encontró %s, pero sin códigos compatibles",
		"%s encontrado, mas sem códigos compatíveis", "%s trouvé, mais aucun code pris en charge",
		"%s gefunden, aber keine unterstützten Codes"},
	{"Loaded %d cheats from %s", "%d trucos cargados de %s", "%d trapaças carregadas de %s", "%d codes chargés depuis %s",
		"%d Cheats aus %s geladen"},
	{"No matching .cht file in /data/snes9x/cheats", "Ningún archivo .cht coincidente en /data/snes9x/cheats",
		"Nenhum arquivo .cht correspondente em /data/snes9x/cheats",
		"Aucun fichier .cht correspondant dans /data/snes9x/cheats", "Keine passende .cht-Datei in /data/snes9x/cheats"},
	// ---- the controller shortcuts
	{"Pause menu", "Menú de pausa", "Menu de pausa", "Menu pause", "Pausenmenü"},
	{"Return to game shelf", "Volver a la estantería de juegos", "Voltar à estante de jogos", "Retour à l'étagère de jeux",
		"Zurück zum Spieleregal"},
	{"Button shortcuts", "Atajos de botones", "Atalhos de botões", "Raccourcis de boutons", "Tastenkürzel"},
	{"Left / Right: Reassign     Cross: Next option     Circle: Back",
		"Izquierda / Derecha: Reasignar     Cruz: Siguiente opción     Círculo: Volver",
		"Esquerda / Direita: Reatribuir     X: Próxima opção     Bola: Voltar",
		"Gauche / Droite : Réattribuer     Croix : Option suivante     Rond : Retour",
		"Links / Rechts: Neu belegen     Kreuz: Nächste Option     Kreis: Zurück"},
	// ---- the shelf
	{"Looking for games...", "Buscando juegos...", "Procurando jogos...", "Recherche des jeux...", "Suche Spiele..."},
	{"Looking for games... %zu found", "Buscando juegos... %zu encontrados", "Procurando jogos... %zu encontrados",
		"Recherche des jeux... %zu trouvés", "Suche Spiele... %zu gefunden"},
	{"Identifying games... %d%% (%zu of %zu)", "Identificando juegos... %d%% (%zu de %zu)",
		"Identificando jogos... %d%% (%zu de %zu)", "Identification des jeux... %d%% (%zu sur %zu)",
		"Erkenne Spiele... %d%% (%zu von %zu)"},
	{"Quit Snes9x PS5?", "¿Salir de Snes9x PS5?", "Sair do Snes9x PS5?", "Quitter Snes9x PS5 ?", "Snes9x PS5 beenden?"},
	{"No games found.", "No se encontraron juegos.", "Nenhum jogo encontrado.", "Aucun jeu trouvé.",
		"Keine Spiele gefunden."},
	{"Copy your SNES ROMs (.sfc .smc .zip ...) to", "Copia tus ROMs de SNES (.sfc .smc .zip ...) en",
		"Copie suas ROMs de SNES (.sfc .smc .zip ...) para", "Copiez vos ROM SNES (.sfc .smc .zip ...) dans",
		"Kopiere deine SNES-ROMs (.sfc .smc .zip ...) nach"},
	{"/data/snes9x/roms   or   snes9x/roms  on a USB drive", "/data/snes9x/roms   o   snes9x/roms  en un USB",
		"/data/snes9x/roms   ou   snes9x/roms  num pendrive", "/data/snes9x/roms   ou   snes9x/roms  sur une clé USB",
		"/data/snes9x/roms   oder   snes9x/roms  auf einem USB-Stick"},
	{"and open Snes9x PS5 again.", "y vuelve a abrir Snes9x PS5.", "e abra o Snes9x PS5 de novo.",
		"puis rouvrez Snes9x PS5.", "und Snes9x PS5 erneut öffnen."},
	{"Downloading covers...", "Descargando portadas...", "Baixando capas...", "Téléchargement des jaquettes...",
		"Lade Cover..."},
	{"Downloading covers... %d left", "Descargando portadas... faltan %d", "Baixando capas... faltam %d",
		"Téléchargement des jaquettes... %d restantes", "Lade Cover... noch %d"},
	{"Downloading covers in the background... %d left", "Descargando portadas en segundo plano... faltan %d",
		"Baixando capas em segundo plano... faltam %d", "Téléchargement des jaquettes en arrière-plan... %d restantes",
		"Lade Cover im Hintergrund... noch %d"},
	{"Offline: covers next time", "Sin conexión: portadas la próxima vez", "Sem conexão: capas na próxima vez",
		"Hors ligne : jaquettes la prochaine fois", "Offline: Cover beim nächsten Mal"},
	{"Retrying %d missing covers (box art, then title screens)",
		"Reintentando %d portadas que faltan (cajas, luego pantallas de título)",
		"Tentando de novo %d capas ausentes (caixas, depois telas de título)",
		"Nouvel essai pour %d jaquettes manquantes (boîtes, puis écrans titres)",
		"Neuer Versuch für %d fehlende Cover (Boxen, dann Titelbilder)"},
	{"no cover", "sin portada", "sem capa", "sans jaquette", "kein Cover"},
	{"Play", "Jugar", "Jogar", "Jouer", "Spielen"},
	{"Browse", "Explorar", "Navegar", "Parcourir", "Blättern"},
	{"Skip 10", "Saltar 10", "Pular 10", "Sauter 10", "10 weiter"},
	{"Get cover", "Obtener portada", "Baixar capa", "Récupérer la jaquette", "Cover holen"},
	{"Fix Covers", "Reparar portadas", "Reparar capas", "Réparer les jaquettes", "Cover reparieren"},
	{"Quit", "Salir", "Sair", "Quitter", "Beenden"},
	// ---- in a game
	{"State loaded from slot %d", "Estado cargado de la ranura %d", "Estado carregado do slot %d",
		"État chargé depuis l'emplacement %d", "Spielstand von Platz %d geladen"},
	{"State saved to slot %d", "Estado guardado en la ranura %d", "Estado salvo no slot %d",
		"État sauvegardé dans l'emplacement %d", "Spielstand auf Platz %d gespeichert"},
	{"Could not save slot %d", "No se pudo guardar la ranura %d", "Não foi possível salvar o slot %d",
		"Impossible de sauvegarder l'emplacement %d", "Platz %d konnte nicht gespeichert werden"},
	{"Slot %d is empty", "La ranura %d está vacía", "O slot %d está vazio", "L'emplacement %d est vide",
		"Platz %d ist leer"},
	{"State slot: %d (used)", "Ranura de estado: %d (usada)", "Slot do estado: %d (usado)", "Emplacement : %d (utilisé)",
		"Spielstand-Platz: %d (belegt)"},
	{"State slot: %d (empty)", "Ranura de estado: %d (vacía)", "Slot do estado: %d (vazio)", "Emplacement : %d (vide)",
		"Spielstand-Platz: %d (leer)"},
	// ---- errors
	{"Could not open the game", "No se pudo abrir el juego", "Não foi possível abrir o jogo", "Impossible d'ouvrir le jeu",
		"Das Spiel konnte nicht geöffnet werden"},
	{"Snes9x PS5 has no access to /data", "Snes9x PS5 no tiene acceso a /data", "O Snes9x PS5 não tem acesso a /data",
		"Snes9x PS5 n'a pas accès à /data", "Snes9x PS5 hat keinen Zugriff auf /data"},
	{"Send Snes9xPS5-v%s.elf with the Payload Manager (it unlocks the app), then open Snes9x PS5 again.",
		"Envía Snes9xPS5-v%s.elf con el Payload Manager (desbloquea la app) y vuelve a abrir Snes9x PS5.",
		"Envie o Snes9xPS5-v%s.elf com o Payload Manager (ele libera o app) e abra o Snes9x PS5 de novo.",
		"Envoyez Snes9xPS5-v%s.elf avec le Payload Manager (il débloque l'app), puis rouvrez Snes9x PS5.",
		"Snes9xPS5-v%s.elf mit dem Payload Manager senden (er entsperrt die App), dann Snes9x PS5 neu öffnen."},
	{"Snes9x: could not open %s", "Snes9x: no se pudo abrir %s", "Snes9x: não foi possível abrir %s",
		"Snes9x : impossible d'ouvrir %s", "Snes9x: %s konnte nicht geöffnet werden"},
	{"Snes9x PS5: video failed to start (see %s/logs/boot.log)",
		"Snes9x PS5: el vídeo no pudo iniciarse (ver %s/logs/boot.log)",
		"Snes9x PS5: o vídeo não conseguiu iniciar (veja %s/logs/boot.log)",
		"Snes9x PS5 : la vidéo n'a pas pu démarrer (voir %s/logs/boot.log)",
		"Snes9x PS5: Video konnte nicht starten (siehe %s/logs/boot.log)"},
	{"Snes9x PS5: the emulator failed to start", "Snes9x PS5: el emulador no pudo iniciarse",
		"Snes9x PS5: o emulador não conseguiu iniciar", "Snes9x PS5 : l'émulateur n'a pas pu démarrer",
		"Snes9x PS5: der Emulator konnte nicht starten"},
	{"Snes9x PS5: downloading 1 cover...", "Snes9x PS5: descargando 1 portada...", "Snes9x PS5: baixando 1 capa...",
		"Snes9x PS5 : téléchargement d'1 jaquette...", "Snes9x PS5: lade 1 Cover..."},
	{"Snes9x PS5: downloading %d covers...", "Snes9x PS5: descargando %d portadas...", "Snes9x PS5: baixando %d capas...",
		"Snes9x PS5 : téléchargement de %d jaquettes...", "Snes9x PS5: lade %d Cover..."},
	// ---- borders
	{"Border", "Marco", "Moldura", "Bordure", "Rahmen"},
	{"Auto", "Auto", "Auto", "Auto", "Auto"},
	{"Default frame", "Marco predeterminado", "Moldura padrão", "Cadre par défaut", "Standardrahmen"},
	{"Download all borders", "Descargar todos los marcos", "Baixar todas as molduras", "Télécharger toutes les bordures",
		"Alle Rahmen herunterladen"},
	{"%d left", "faltan %d", "faltam %d", "%d restantes", "noch %d"},
	{"%d queued", "%d en cola", "%d na fila", "%d en attente", "%d eingereiht"},
	{"Nothing to fetch", "Nada que descargar", "Nada para baixar", "Rien à télécharger", "Nichts zu laden"},
	{"Waiting for the helper", "Esperando al helper", "Aguardando o helper", "En attente du helper", "Warte auf den Helper"},
	{"%d fetched", "%d descargados", "%d baixadas", "%d téléchargées", "%d geladen"},
	// ---- the shelf's tabs and the Network tab
	{"Internal", "Interno", "Interno", "Interne", "Intern"},
	{"Network", "Red", "Rede", "Réseau", "Netzwerk"},
	{"Tabs", "Pestañas", "Abas", "Onglets", "Tabs"},
	{"Scan again", "Buscar de nuevo", "Buscar de novo", "Réanalyser", "Erneut suchen"},
	{"Scanning the network shares...", "Buscando en los recursos de red...", "Verificando os compartilhamentos de rede...",
		"Analyse des partages réseau...", "Durchsuche die Netzwerkfreigaben..."},
	{"No network shares yet.", "Aún no hay recursos de red.", "Ainda não há compartilhamentos de rede.",
		"Aucun partage réseau pour l'instant.", "Noch keine Netzwerkfreigaben."},
	{"Looking for network games...", "Buscando juegos en la red...", "Procurando jogos na rede...",
		"Recherche des jeux sur le réseau...", "Suche Spiele im Netzwerk..."},
	{"Could not reach the network share.", "No se pudo acceder al recurso de red.",
		"Não foi possível acessar o compartilhamento de rede.", "Impossible de joindre le partage réseau.",
		"Die Netzwerkfreigabe ist nicht erreichbar."},
	{"Could not reach the network shares.", "No se pudo acceder a los recursos de red.",
		"Não foi possível acessar os compartilhamentos de rede.", "Impossible de joindre les partages réseau.",
		"Die Netzwerkfreigaben sind nicht erreichbar."},
	{"No games found on the network share.", "No se encontraron juegos en el recurso de red.",
		"Nenhum jogo encontrado no compartilhamento de rede.", "Aucun jeu trouvé sur le partage réseau.",
		"Keine Spiele auf der Netzwerkfreigabe gefunden."},
	{"No games found on the network shares.", "No se encontraron juegos en los recursos de red.",
		"Nenhum jogo encontrado nos compartilhamentos de rede.", "Aucun jeu trouvé sur les partages réseau.",
		"Keine Spiele auf den Netzwerkfreigaben gefunden."},
	{"Connecting to %s... %d s", "Conectando con %s... %d s", "Conectando a %s... %d s", "Connexion à %s... %d s",
		"Verbinde mit %s... %d s"},
	{"%s: connected, no ROMs found.", "%s: conectado, no se encontraron ROMs.", "%s: conectado, nenhuma ROM encontrada.",
		"%s : connecté, aucune ROM trouvée.", "%s: verbunden, keine ROMs gefunden."},
	{"%s is not available.", "%s no está disponible.", "%s não está disponível.", "%s n'est pas disponible.",
		"%s ist nicht verfügbar."},
	{"and %d more", "y %d más", "e mais %d", "et %d de plus", "und %d weitere"},
	{"Add one:", "Añade uno:", "Adicione um:", "Ajoutez-en un :", "Eine hinzufügen:"},
	{"Settings > Network shares", "Ajustes > Recursos de red", "Configurações > Compartilhamentos de rede",
		"Paramètres > Partages réseau", "Einstellungen > Netzwerkfreigaben"},
	{"SMB or NFS; the pad types the address, user and password.",
		"SMB o NFS; con el mando se escriben la dirección, el usuario y la contraseña.",
		"SMB ou NFS; o controle digita o endereço, o usuário e a senha.",
		"SMB ou NFS ; la manette saisit l'adresse, l'utilisateur et le mot de passe.",
		"SMB oder NFS; Adresse, Benutzer und Passwort tippt der Controller."},
	{"Games on a NAS or PC?", "¿Juegos en un NAS o PC?", "Jogos num NAS ou PC?", "Des jeux sur un NAS ou un PC ?",
		"Spiele auf einem NAS oder PC?"},
	{"Games on a NAS or PC?  Triangle (Settings), then Network shares.",
		"¿Juegos en un NAS o PC?  Triángulo (Ajustes), luego Recursos de red.",
		"Jogos num NAS ou PC?  Triângulo (Configurações), depois Compartilhamentos de rede.",
		"Des jeux sur un NAS ou un PC ?  Triangle (Paramètres), puis Partages réseau.",
		"Spiele auf einem NAS oder PC?  Dreieck (Einstellungen), dann Netzwerkfreigaben."},
	{"Network: %s", "Red: %s", "Rede: %s", "Réseau : %s", "Netzwerk: %s"},
	// ---- the Network shares screen
	{"Network shares", "Recursos de red", "Compartilhamentos de rede", "Partages réseau", "Netzwerkfreigaben"},
	{"None", "Ninguno", "Nenhum", "Aucun", "Keine"},
	{"%d of %d connected", "%d de %d conectados", "%d de %d conectados", "%d sur %d connectés", "%d von %d verbunden"},
	{"Games on your network (SMB or NFS)", "Juegos en tu red (SMB o NFS)", "Jogos na sua rede (SMB ou NFS)",
		"Jeux sur votre réseau (SMB ou NFS)", "Spiele in deinem Netzwerk (SMB oder NFS)"},
	{"Add a share", "Añadir un recurso", "Adicionar um compartilhamento", "Ajouter un partage", "Freigabe hinzufügen"},
	{"Edit share", "Editar el recurso", "Editar o compartilhamento", "Modifier le partage", "Freigabe bearbeiten"},
	{"Connect again", "Volver a conectar", "Conectar de novo", "Reconnecter", "Erneut verbinden"},
	{"Connecting to every share again.", "Conectando de nuevo con todos los recursos.",
		"Conectando de novo a todos os compartilhamentos.", "Reconnexion à tous les partages.",
		"Verbinde alle Freigaben erneut."},
	{"No shares yet. Add one: the games on it show up in the list, saves stay on the console.",
		"Aún no hay recursos. Añade uno: sus juegos aparecen en la lista y las partidas guardadas se quedan en la consola.",
		"Ainda não há compartilhamentos. Adicione um: os jogos dele aparecem na lista e os saves ficam no console.",
		"Aucun partage pour l'instant. Ajoutez-en un : ses jeux apparaissent dans la liste, les sauvegardes restent sur la console.",
		"Noch keine Freigaben. Füge eine hinzu: Ihre Spiele erscheinen in der Liste, Spielstände bleiben auf der Konsole."},
	{"%d line of shares.txt is not a valid address and is skipped.",
		"%d línea de shares.txt no es una dirección válida y se omite.",
		"%d linha do shares.txt não é um endereço válido e é ignorada.",
		"%d ligne de shares.txt n'est pas une adresse valide et est ignorée.",
		"%d Zeile in shares.txt ist keine gültige Adresse und wird übersprungen."},
	{"%d lines of shares.txt are not valid addresses and are skipped.",
		"%d líneas de shares.txt no son direcciones válidas y se omiten.",
		"%d linhas do shares.txt não são endereços válidos e são ignoradas.",
		"%d lignes de shares.txt ne sont pas des adresses valides et sont ignorées.",
		"%d Zeilen in shares.txt sind keine gültigen Adressen und werden übersprungen."},
	{"Up to %d shares are used. Remove one first.", "Ya se usan %d recursos, el máximo. Quita uno primero.",
		"Já há %d compartilhamentos em uso, o máximo. Remova um antes.",
		"%d partages au maximum sont utilisés. Supprimez-en un d'abord.",
		"Schon %d Freigaben (das Maximum) in Gebrauch. Entferne zuerst eine."},
	{"Connecting...", "Conectando...", "Conectando...", "Connexion...", "Verbinde..."},
	{"Connected", "Conectado", "Conectado", "Connecté", "Verbunden"},
	{"Not connected", "No conectado", "Não conectado", "Non connecté", "Nicht verbunden"},
	{"Connection lost", "Conexión perdida", "Conexão perdida", "Connexion perdue", "Verbindung verloren"},
	{"Not used", "Sin usar", "Não usado", "Inutilisé", "Nicht genutzt"},
	{"Choose", "Elegir", "Escolher", "Choisir", "Auswählen"},
	{"Edit", "Editar", "Editar", "Modifier", "Bearbeiten"},
	{"Change", "Cambiar", "Mudar", "Changer", "Ändern"},
	// ---- the share form
	{"Share type", "Tipo", "Tipo", "Type", "Typ"},
	{"Server", "Servidor", "Servidor", "Serveur", "Server"},
	{"IP address, like 192.168.1.50", "Dirección IP, como 192.168.1.50", "Endereço IP, como 192.168.1.50",
		"Adresse IP, comme 192.168.1.50", "IP-Adresse, z. B. 192.168.1.50"},
	{"Share name", "Nombre del recurso", "Nome do compartilhamento", "Nom du partage", "Freigabename"},
	{"required", "obligatorio", "obrigatório", "obligatoire", "erforderlich"},
	{"Folder in the share", "Carpeta en el recurso", "Pasta no compartilhamento", "Dossier dans le partage",
		"Ordner in der Freigabe"},
	{"the whole share", "todo el recurso", "o compartilhamento inteiro", "tout le partage", "die ganze Freigabe"},
	{"Export path", "Ruta de exportación", "Caminho de exportação", "Chemin d'export", "Exportpfad"},
	{"like /export/snes", "como /export/snes", "como /export/snes", "comme /export/snes", "z. B. /export/snes"},
	{"User name", "Usuario", "Usuário", "Nom d'utilisateur", "Benutzername"},
	{"Guest", "Invitado", "Convidado", "Invité", "Gast"},
	{"Password", "Contraseña", "Senha", "Mot de passe", "Passwort"},
	{"Domain", "Dominio", "Domínio", "Domaine", "Domäne"},
	{"SMB version", "Versión de SMB", "Versão do SMB", "Version SMB", "SMB-Version"},
	{"Signing", "Firma", "Assinatura", "Signature", "Signierung"},
	{"Encryption", "Cifrado", "Criptografia", "Chiffrement", "Verschlüsselung"},
	{"NFS version", "Versión de NFS", "Versão do NFS", "Version NFS", "NFS-Version"},
	{"User ID", "ID de usuario", "ID de usuário", "ID utilisateur", "Benutzer-ID"},
	{"Group ID", "ID de grupo", "ID de grupo", "ID de groupe", "Gruppen-ID"},
	{"Default", "Predeterminado", "Padrão", "Par défaut", "Standard"},
	{"Test connection", "Probar la conexión", "Testar a conexão", "Tester la connexion", "Verbindung testen"},
	{"Save", "Guardar", "Salvar", "Enregistrer", "Speichern"},
	{"Remove this share", "Quitar este recurso", "Remover este compartilhamento", "Supprimer ce partage",
		"Diese Freigabe entfernen"},
	{"Cancel", "Cancelar", "Cancelar", "Annuler", "Abbrechen"},
	{"Use the server's IP address: names often do not resolve on the console.",
		"Usa la dirección IP del servidor: en la consola los nombres a menudo no se resuelven.",
		"Use o endereço IP do servidor: no console, os nomes muitas vezes não são resolvidos.",
		"Utilisez l'adresse IP du serveur : sur la console, les noms ne sont souvent pas résolus.",
		"Nutze die IP-Adresse des Servers: Namen werden auf der Konsole oft nicht aufgelöst."},
	{"Testing the connection", "Probando la conexión", "Testando a conexão", "Test de la connexion", "Teste die Verbindung"},
	{"Stop the test", "Detener la prueba", "Parar o teste", "Arrêter le test", "Test abbrechen"},
	{"Test stopped.", "Prueba detenida.", "Teste interrompido.", "Test arrêté.", "Test abgebrochen."},
	{"Discard your changes?", "¿Descartar los cambios?", "Descartar as alterações?", "Abandonner les modifications ?",
		"Änderungen verwerfen?"},
	{"Discard", "Descartar", "Descartar", "Abandonner", "Verwerfen"},
	{"Keep editing", "Seguir editando", "Continuar editando", "Continuer", "Weiter bearbeiten"},
	{"Remove this share?", "¿Quitar este recurso?", "Remover este compartilhamento?", "Supprimer ce partage ?",
		"Diese Freigabe entfernen?"},
	{"Remove", "Quitar", "Remover", "Supprimer", "Entfernen"},
	{"Keep it", "Conservarlo", "Manter", "Le garder", "Behalten"},
	// ---- the on-screen keyboard (its title says what the field is)
	{"Server: an IP address like 192.168.1.50", "Servidor: una dirección IP como 192.168.1.50",
		"Servidor: um endereço IP como 192.168.1.50", "Serveur : une adresse IP comme 192.168.1.50",
		"Server: eine IP-Adresse wie 192.168.1.50"},
	{"Folder in the share (empty: the whole share)", "Carpeta en el recurso (vacía: todo el recurso)",
		"Pasta no compartilhamento (vazia: o compartilhamento inteiro)", "Dossier dans le partage (vide : tout le partage)",
		"Ordner in der Freigabe (leer: die ganze Freigabe)"},
	{"Export path, like /export/snes", "Ruta de exportación, como /export/snes", "Caminho de exportação, como /export/snes",
		"Chemin d'export, comme /export/snes", "Exportpfad, z. B. /export/snes"},
	{"User name (empty: guest)", "Usuario (vacío: invitado)", "Usuário (vazio: convidado)",
		"Nom d'utilisateur (vide : invité)", "Benutzername (leer: Gast)"},
	{"Domain (usually empty)", "Dominio (normalmente vacío)", "Domínio (geralmente vazio)", "Domaine (en général vide)",
		"Domäne (meist leer)"},
	{"User ID (a number, empty: default)", "ID de usuario (un número, vacío: predeterminado)",
		"ID de usuário (um número, vazio: padrão)", "ID utilisateur (un nombre, vide : par défaut)",
		"Benutzer-ID (eine Zahl, leer: Standard)"},
	{"Group ID (a number, empty: default)", "ID de grupo (un número, vacío: predeterminado)",
		"ID de grupo (um número, vazio: padrão)", "ID de groupe (un nombre, vide : par défaut)",
		"Gruppen-ID (eine Zahl, leer: Standard)"},
	{"%zu character", "%zu carácter", "%zu caractere", "%zu caractère", "%zu Zeichen"},
	{"%zu characters", "%zu caracteres", "%zu caracteres", "%zu caractères", "%zu Zeichen"},
	{"Hidden as you type.", "Se oculta al escribir.", "Oculta ao digitar.", "Masqué pendant la saisie.",
		"Beim Tippen verborgen."},
	{"Type", "Escribir", "Digitar", "Saisir", "Tippen"},
	{"Delete", "Borrar", "Apagar", "Effacer", "Löschen"},
	{"Space", "Espacio", "Espaço", "Espace", "Leertaste"},
	{"Shift", "Mayús", "Shift", "Maj", "Umschalt"},
	{"Cursor", "Cursor", "Cursor", "Curseur", "Cursor"},
	{"Done", "Listo", "Pronto", "OK", "Fertig"},
};

const char* const kLanguageNames[] = {"English", "Español", "Português (Brasil)", "Français", "Deutsch"};

const std::unordered_map<std::string, const Entry*>& Index()
{
	static const std::unordered_map<std::string, const Entry*> index = [] {
		std::unordered_map<std::string, const Entry*> m;
		for (const Entry& e : kTable)
			m.emplace(e.en, &e);
		return m;
	}();
	return index;
}
} // namespace

const char* LanguageName(int lang)
{
	return lang >= 0 && lang < int(Language::Count) ? kLanguageNames[lang] : kLanguageNames[0];
}

const char* T(const char* english)
{
	const int lang = Config().language;
	if (lang <= 0 || lang >= int(Language::Count) || !english)
		return english;
	const auto& index = Index();
	const auto it = index.find(english);
	if (it == index.end())
	{
#ifndef __PROSPERO__
		// the host tests: every text shown without a translation, once (SNES9X_HOST_I18N_MISSING=<file>)
		if (const char* path = getenv("SNES9X_HOST_I18N_MISSING"))
		{
			static std::mutex m;
			static std::set<std::string> seen;
			std::lock_guard<std::mutex> lock(m);
			if (seen.insert(english).second)
				if (FILE* f = fopen(path, "a"))
				{
					fprintf(f, "%s\n", english);
					fclose(f);
				}
		}
#endif
		return english;
	}
	const Entry& e = *it->second;
	const char* t = lang == 1 ? e.es : lang == 2 ? e.pt : lang == 3 ? e.fr : e.de;
	return t && *t ? t : english;
}
} // namespace fe
