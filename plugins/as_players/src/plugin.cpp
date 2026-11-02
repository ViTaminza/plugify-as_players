// as_players — Plugify C++ port of the Pisex Metamod:Source module "Players" (as_players).
//
// Admin player actions: mm_kill, mm_kick, mm_slap, mm_rename, mm_changeteam, mm_switchteam,
// mm_noclip, mm_who, plus the "players" admin-menu category with the same eight items (who,
// noclip, switchteam, changeteam, rename, slap, kick, kill), a player picker, a team picker and
// a damage picker. Commands, menu items, phrases, permission strings and the action parameters
// sent to the admin core (SendAction) are kept from the original.
//
// Pure s2sdk, no pisexcompat and no s2guard (docs/PLUGIN_STANDARD.md §7). Every guard the shared
// libraries used to provide lives in this file:
//
//   * PITFALLS B1/B2 — the map gate g_mapReady. s2sdk client functions dereference null before
//     the first map and, worse, an emptied client list between OnMapEnd and the next OnMapStart.
//     The map listeners are registered in OnPluginStart; readiness is latched by OnMapStart and
//     by the first OnPluginUpdate of a plugin loaded into a running map (unless an OnMapEnd was
//     seen since OnPluginStart), and dropped by OnMapEnd. No client, entity or schema call is
//     made while it is closed. OnMapEnd only clears state (it does not touch a client).
//   * PITFALLS C2 — every client query goes through CanQueryClient/HasClient: slot 0..63,
//     IsClientConnected && IsClientInGame && !IsClientSourceTV. Before a controller/pawn is used
//     the entity handle is validated: CEntityHandle is a *signed* int32 (index | serial << 15)
//     whose serial may set the sign bit, so negative values are valid handles. Only 0 and -1 are
//     sentinels; every other int32, including negative, is passed to the engine's
//     IsValidEntHandle. Entity *indices* and player slots stay positive and are never mixed with
//     handles. The pawn pointer is resolved to a positive entity index. s2sdk's "<unknown>" name
//     becomes "".
//     A bot's SteamID64 is never read (PITFALLS B5); it is 0.
//   * Permissions — every client-originated action goes through HasReadyPermission():
//     IsClientAdminDataReady(slot, live SteamID64) must be true before HasPermission() is
//     trusted, because until the admin core's asynchronous SELECT answers (or after a failed
//     one, or on an admin_system older than 1.1.0 where the export is missing) HasPermission()
//     can still hold the rights of the slot's previous occupant. Pending/failed/missing denies
//     (fail soft), it never opens the action up (plugins/admin_system/API.md §4.1). Only slot
//     -1 is the server-console bypass; -2 and out-of-range callers fail closed.
//   * PITFALLS C11 — the eight commands are console commands of this plugin
//     (ClientCanExecute|Release, so a client console reaches the permission check exactly like
//     the Pisex RegCommand they replace). s2sdk dispatches a chat "!mm_kill" to the same callback
//     with ConCommandContext::Chat; these commands are console-only, so the callback answers
//     Continue for chat and lets s2sdk treat the line as ordinary chat. AddConsoleCommand answers
//     false when the name already existed and then only attaches a listener: OnPluginEnd removes
//     a name this plugin created with RemoveCommand and never calls RemoveCommand on somebody
//     else's command.
//   * PITFALLS C10 — menus. The plugin uses the s2sdk menu manager directly: MenuId 0 is also
//     "no menu" in the client state, so a menu created with id 0 is burnt and re-created; every
//     menu this plugin opens is tracked per slot and destroyed after its session ends (the
//     manager never destroys a closed menu), and DestroyMenu is never called from inside a menu
//     handler. All engine work triggered by a selection (kick/kill/names/team/chat/action) is
//     deferred to OnPluginUpdate, i.e. it never runs under the menu manager's mutex. Destruction
//     is fail-safe: a display that survives CancelClientMenu(Disconnect) in a map-down window is
//     switched to the builtin "chat"/"console" no-op-close type (verified with GetMenuType) before
//     DestroyMenu; if even that is impossible the request is parked and A7_UNRESOLVED is logged,
//     never a blind backend close on the torn-down client.
//   * PITFALLS C9 — no timers at all: the only time-based feature is the menu manager's own
//     DisplayMenu() timeout (60 s, the cs2-menus "MenuTime" default of the original), so no
//     engine action can run under the s2sdk timer mutex either.
//
// The admin core stays a plugin API dependency: the generated __admin_system_* binding globals
// and the hand-written include/admin_api.hpp wrapper (plugins/admin_system/include) are compiled
// into this plugin, exactly like every other admin_system module. The hudmenu plugin is an
// *optional* plugin API dependency for the per-player menu type (its exported
// GetClientMenuType(), the original's "!menu" choice) - a real plugin API, not a wrapper
// library; without it the menu type falls back to the exact Pisex table (HUD_LAYOUT -> "hud"
// when its backend is registered, otherwise "button"). The plugin does not read a
// config, a translation file or a database of its own: phrases come from admin_system's
// GetTranslation() (the original carried them in addons/translations/admin_system.phrases.txt,
// which the core owns), so there is no ServerLang/core.cfg logic to duplicate here.
//
// Slot identity: every action that outlives a menu selection carries the target's SteamID64
// (or "this slot must still be a bot" for a bot) and is re-resolved against the live client
// table when it runs, so a player who left and a slot that was reused by somebody else can
// never turn a delayed selection into an action on the wrong player (PITFALLS C2/B4).
//
// Nothing touches a client in OnPluginStart; OnPluginStart never returns Failed (PITFALLS A1:
// that stops every Plugify plugin), and every registration is removed in OnPluginEnd (A7). The
// admin core has no item-unregister API, so the eight "players" items are re-registered through
// the low-level binding with null callbacks before the library goes away - after the unload the
// core holds no function pointer into this .so (the category was registered with a null display
// callback from the start). `plg reload` therefore leaves nothing pointing into the library.

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plg/vector.hpp>
#include <plugin_export.h>

#include <s2sdk/s2sdk.hpp>

#include <hudmenu/hudmenu.hpp>

#include <admin_api.hpp>

#include "text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr const char* kLogTag = "Players";
// CS2 has 64 player slots; the original iterated 0..63 without asking for a maximum.
constexpr int32_t kSlots = 64;
// The original's cs2-menus menus are paginated five items per page (the chat menu of Pisex).
constexpr int32_t kItemsPerPage = 5;
// The cs2-menus core.ini "MenuTime" default of the original: a menu closes itself after 60 s.
constexpr int32_t kMenuTimeoutSeconds = 60;
constexpr int kRandomNameLength = 10;

// ---------------------------------------------------------------------------------------------
// Interfaces (the original's g_pAdminApi global).
// ---------------------------------------------------------------------------------------------
admin_system::AdminApi* g_pAdminApi = nullptr;
// True when every admin_system export this module calls has a binding. A missing admin_system
// must not crash the module; it degrades to "no permission, no menu items" (PITFALLS A1/A4).
bool g_adminBindingsAvailable = false;
// True when every s2sdk menu export this module calls has a binding. Without them the admin
// menu items are not registered at all (rather than showing entries that do nothing).
bool g_menuBindingsAvailable = false;
// The hudmenu plugin is an *optional* plugin API dependency (a legitimate API, not a wrapper
// library): its GetClientMenuType() is the original's per-player "!menu" choice ("hud"/"wasd"/
// "button"/"centerhtml"/"chat"). The binding is probed at every use (not cached): the optional
// plugin may be loaded or reloaded after this one. When it is absent the plugin uses the Pisex
// HUD_LAYOUT default: "hud" when its backend is registered, otherwise "button" (the original's
// exact fallback - alive-players-only, not the centerhtml convenience).

// ---------------------------------------------------------------------------------------------
// PITFALLS B1/B2: the map gate.
// ---------------------------------------------------------------------------------------------
bool g_mapReady = false;		   // a map is up: client/entity/schema calls are safe
bool g_mapEndedSinceStart = false; // OnMapEnd was seen since OnPluginStart
bool g_firstUpdateDone = false;	   // the mid-map latch has already been decided
bool g_mapListeners = false;	   // OnMapStart/OnMapEnd are registered with s2sdk

// True while the eight "players" items are registered in the admin core. OnPluginEnd uses it to
// retire them (the core has no unregister API, see RetireAdminMenuItems below).
bool g_adminMenuItemsRegistered = false;

// ---------------------------------------------------------------------------------------------
// Logging (engine-free when plg::Log is unavailable; see PLUGIFY_PLUGIN).
// ---------------------------------------------------------------------------------------------
void LogMessage(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

void LogError(std::string_view message) {
	LogMessage(std::string("[") + kLogTag + "] " + std::string(message), plg::Severity::Error);
}

void LogInfo(std::string_view message) {
	LogMessage(std::string("[") + kLogTag + "] " + std::string(message), plg::Severity::Info);
}

// Translated phrase from the admin core; the key itself when the core is not loaded (the
// original's g_vecPhrases lookup answered with the key on a miss).
const char* T(const char* key) {
	if (g_pAdminApi != nullptr) {
		return g_pAdminApi->GetTranslation(key);
	}
	return key;
}

const char* Safe(const char* text) {
	return text != nullptr ? text : "";
}

plg::string Plg(std::string_view text) {
	return text.empty() ? plg::string() : plg::string(text.data(), text.size());
}

std::string ToStd(const plg::string& text) {
	return std::string(text.data(), text.size());
}

// ---------------------------------------------------------------------------------------------
// Binding availability
// ---------------------------------------------------------------------------------------------
// The exports used by this module. A dependency is loaded before its consumer, so normally all
// of them are filled; the checks keep the module alive (and honest about what it can do) when a
// plugin was loaded in an unexpected order or an older admin_system is installed.
bool AdminApiAvailable() {
	return __admin_system_GetPluginVersion != nullptr && __admin_system_GetTranslation != nullptr &&
		   __admin_system_GetMessageType != nullptr && __admin_system_IsCoreLoaded != nullptr &&
		   __admin_system_HasPermission != nullptr && __admin_system_GetAdminFlags != nullptr &&
		   __admin_system_GetFlagName != nullptr && __admin_system_SendAction != nullptr &&
		   __admin_system_RegisterCategory != nullptr && __admin_system_RegisterItem != nullptr &&
		   __admin_system_OnCoreLoaded_Register != nullptr &&
		   __admin_system_OnCoreLoaded_Unregister != nullptr &&
		   __admin_system_ShowAdminLastCategoryMenu != nullptr;
}

bool MenuApiAvailable() {
	// DestroyMenu/GetClientMenu/CancelClientMenu are required for every menu handle, and
	// SetMenuType/GetMenuType are required *before* any menu is created: they are the fail-safe
	// that makes a still-displayed menu closable with a no-op backend (see DestroyMenuChecked).
	return __s2sdk_CreateMenu != nullptr && __s2sdk_DestroyMenu != nullptr &&
		   __s2sdk_AddMenuItem != nullptr && __s2sdk_DisplayMenu != nullptr &&
		   __s2sdk_SetMenuPagination != nullptr && __s2sdk_SetMenuExitButton != nullptr &&
		   __s2sdk_SetMenuExitBackButton != nullptr && __s2sdk_SetMenuCloseOnSelect != nullptr &&
		   __s2sdk_GetClientMenu != nullptr && __s2sdk_GetClientMenuOffset != nullptr &&
		   __s2sdk_CancelClientMenu != nullptr && __s2sdk_SetMenuType != nullptr &&
		   __s2sdk_GetMenuType != nullptr;
}

bool HasPermission(int32_t iSlot, const char* szPermission) {
	if (!g_adminBindingsAvailable || g_pAdminApi == nullptr || szPermission == nullptr) {
		return false;
	}
	return g_pAdminApi->HasPermission(iSlot, szPermission);
}

// The admin-load readiness of API 1.1.0 (see plugins/admin_system/API.md §4.1): false means
// "ask again later", not "not an admin". Showing admin data or acting on a permission while the
// SELECT is still pending would trust a record that may belong to the slot's previous occupant.
bool AdminDataReady(int32_t iSlot, uint64_t iSteamID64) {
	if (g_pAdminApi == nullptr || iSteamID64 == 0 || __admin_system_IsClientAdminDataReady == nullptr) {
		return false;
	}
	return g_pAdminApi->IsClientAdminDataReady(iSlot, iSteamID64);
}

// ---------------------------------------------------------------------------------------------
// PITFALLS C2: the client guards.
// ---------------------------------------------------------------------------------------------
// The map gate and the slot range are the only requirements of a function that addresses a slot
// without reading its state (a print).
bool CanQueryClient(int32_t iSlot) {
	return g_mapReady && iSlot >= 0 && iSlot < kSlots;
}

// A live, human-or-bot slot: connected, in game and not the SourceTV proxy.
bool HasClient(int32_t iSlot) {
	if (!CanQueryClient(iSlot) || !__s2sdk_IsClientConnected || !__s2sdk_IsClientInGame ||
		!__s2sdk_IsClientSourceTV) {
		return false;
	}
	return s2sdk::IsClientConnected(iSlot) && s2sdk::IsClientInGame(iSlot) &&
		   !s2sdk::IsClientSourceTV(iSlot);
}

bool IsBot(int32_t iSlot) {
	return HasClient(iSlot) && __s2sdk_IsFakeClient != nullptr && s2sdk::IsFakeClient(iSlot);
}

// The controller entity handle of the slot, or -1 when there is none. CEntityHandle is a SIGNED
// int32: index | serial << 15, where the serial may legitimately set the sign bit, so negative
// values are valid handles. Only 0 and -1 are sentinels (s2sdk's invalid handle and the world
// entity, which is never a player controller); every other int32 is handed to the engine's
// IsValidEntHandle (PITFALLS C1/C2). The same signed value is used as the identity of a deferred
// action: a slot whose controller handle changed belongs to a new connection, even when the
// SteamID64 happens to be the same.
int32_t ControllerHandle(int32_t iSlot) {
	if (!HasClient(iSlot) || !__s2sdk_PlayerSlotToEntHandle || !__s2sdk_IsValidEntHandle) {
		return -1;
	}
	const int32_t iHandle = s2sdk::PlayerSlotToEntHandle(iSlot);
	if (iHandle == 0 || iHandle == -1 || !s2sdk::IsValidEntHandle(iHandle)) {
		return -1;
	}
	return iHandle;
}

bool ControllerValid(int32_t iSlot) {
	return ControllerHandle(iSlot) != -1;
}

// The pawn entity index of the slot (0 = no pawn: dead, connecting, or not a player slot).
int32_t PawnIndex(int32_t iSlot) {
	if (!ControllerValid(iSlot) || !__s2sdk_GetClientPawn || !__s2sdk_EntPointerToEntIndex) {
		return 0;
	}
	void* pPawn = s2sdk::GetClientPawn(iSlot);
	if (pPawn == nullptr) {
		return 0;
	}
	const int32_t iIndex = s2sdk::EntPointerToEntIndex(pPawn);
	return iIndex > 0 ? iIndex : 0;
}

bool PawnValid(int32_t iSlot) {
	return PawnIndex(iSlot) > 0;
}

// s2sdk answers "<unknown>" for a name it could not resolve; the original printed "".
std::string ClientName(int32_t iSlot) {
	if (!ControllerValid(iSlot) || !__s2sdk_GetClientName) {
		return {};
	}
	const plg::string szName = s2sdk::GetClientName(iSlot);
	std::string out = ToStd(szName);
	if (out == "<unknown>") {
		out.clear();
	}
	return out;
}

// 0 for a bot, an unauthenticated client, a disconnected slot and while no map is up.
uint64_t ClientSteamID64(int32_t iSlot) {
	if (!HasClient(iSlot) || IsBot(iSlot) || !__s2sdk_GetClientSteamID64) {
		return 0;
	}
	return s2sdk::GetClientSteamID64(iSlot);
}

// The permission check every client-originated action uses. IsClientAdminDataReady() must be
// true for the *live* SteamID64 of the slot first: until the asynchronous admin SELECT answers
// (or after a failed one, or on an admin_system older than 1.1.0 where the export is missing)
// HasPermission() may still hold the previous occupant's rights. Pending/failed/missing means
// "deny now, ask again later" - never a fail-open (API.md §4.1).
bool HasReadyPermission(int32_t iSlot, const char* szPermission) {
	if (iSlot < 0 || szPermission == nullptr || __admin_system_IsClientAdminDataReady == nullptr) {
		return false;
	}
	const uint64_t iSteamID64 = ClientSteamID64(iSlot);
	if (iSteamID64 == 0 || !AdminDataReady(iSlot, iSteamID64)) {
		return false;
	}
	return HasPermission(iSlot, szPermission);
}

bool ClientAlive(int32_t iSlot) {
	return CanQueryClient(iSlot) && __s2sdk_IsClientAlive != nullptr && s2sdk::IsClientAlive(iSlot);
}

// Reads the pawn's move type; false when the value cannot be read (no binding / no pawn), so the
// caller never presents a made-up "OFF".
bool ClientNoclip(int32_t iSlot, bool& bNoclipOut) {
	if (!PawnValid(iSlot) || !__s2sdk_GetClientMoveType) {
		return false;
	}
	bNoclipOut = s2sdk::GetClientMoveType(iSlot) == s2sdk::MoveType::Noclip;
	return true;
}

// The output name of the admin who acted: "Console" for the server console, the live name
// otherwise (empty when the client is gone - the message is then shown with an empty name,
// exactly like the original's GetPlayerName()).
std::string ActorName(int32_t iSlot) {
	if (iSlot == -1) {
		return "Console";
	}
	if (iSlot < 0) {
		return {}; // -2/out-of-range is not a caller and not the console
	}
	return ClientName(iSlot);
}

// ---------------------------------------------------------------------------------------------
// Output: the Pisex printers over s2sdk (chat, the caller's console, the server console).
// ---------------------------------------------------------------------------------------------
// The server console: colour tags are meaningless there and are stripped (pisexcompat did the
// same); PrintToServer() is ConMsg("%s\n"), so one trailing newline is dropped to keep one line.
void PrintServerLine(std::string_view text) {
	if (!__s2sdk_PrintToServer) {
		return;
	}
	std::string line = as_players::StripColorTags(text);
	if (!line.empty() && line.back() == '\n') {
		line.pop_back();
	}
	s2sdk::PrintToServer(Plg(line));
}

// A console message: the server console for a negative slot, the caller's own console
// otherwise. A client console gets the text as the original sent it (colour tags included,
// like the Pisex PrintToConsole); only the server console is stripped of them.
void PrintConsole(int32_t iSlot, std::string_view text) {
	if (iSlot == -1) {
		PrintServerLine(text);
		return;
	}
	if (iSlot < 0 || !CanQueryClient(iSlot) || !__s2sdk_PrintToConsole) {
		return; // -2/out-of-range is invalid, not the server console (fail closed)
	}
	s2sdk::PrintToConsole(iSlot, Plg(text));
}

void PrintChat(int32_t iSlot, std::string_view text) {
	if (iSlot == -1) {
		PrintServerLine(text);
		return;
	}
	if (iSlot < 0 || !CanQueryClient(iSlot) || !__s2sdk_PrintToChat) {
		return; // -2/out-of-range is invalid, not the server console (fail closed)
	}
	s2sdk::PrintToChat(iSlot, Plg(text));
}

void PrintChatAll(std::string_view text) {
	if (!g_mapReady || !__s2sdk_PrintToChatAll) {
		return;
	}
	s2sdk::PrintToChatAll(Plg(text));
}

// The translated phrase with printf arguments, colourised for chat.
void PrintChatPhrase(int32_t iSlot, const char* szKey, ...) {
	va_list args;
	va_start(args, szKey);
	const std::string text = as_players::FormatV(T(szKey), args);
	va_end(args);
	PrintChat(iSlot, as_players::Colorize(text));
}

void PrintChatAllPhrase(const char* szKey, ...) {
	va_list args;
	va_start(args, szKey);
	const std::string text = as_players::FormatV(T(szKey), args);
	va_end(args);
	PrintChatAll(as_players::Colorize(text));
}

// A printf-formatted, already-known string in chat (flag names are data, not a format string).
void PrintChatText(int32_t iSlot, const std::string& text) {
	PrintChat(iSlot, as_players::Colorize(text));
}

// The original's "usage message to a console or a player" block. Only slot -1 is the server
// console: any other negative slot is invalid and gets no output at all.
void PrintUsage(int32_t iSlot, const char* szPhrase, const char* szCommand) {
	PrintConsole(iSlot, as_players::Format(T(szPhrase), Safe(szCommand)));
}

// ---------------------------------------------------------------------------------------------
// The original's CCommand parsing (the Pisex handlers tokenise the raw argument string).
// ---------------------------------------------------------------------------------------------
class CommandArgs {
public:
	void Tokenize(const char* text) {
		text_.assign(text != nullptr ? text : "");
		args_.clear();
		std::size_t i = 0;
		const std::size_t size = text_.size();
		while (i < size) {
			while (i < size && std::isspace(static_cast<unsigned char>(text_[i]))) {
				++i;
			}
			if (i >= size) {
				break;
			}
			std::string token;
			if (text_[i] == '"') {
				++i;
				while (i < size && text_[i] != '"') {
					token.push_back(text_[i++]);
				}
				if (i < size) {
					++i; // closing quote
				}
			} else {
				while (i < size && !std::isspace(static_cast<unsigned char>(text_[i]))) {
					token.push_back(text_[i++]);
				}
			}
			args_.push_back(std::move(token));
		}
	}

	int ArgC() const { return static_cast<int>(args_.size()); }

	const char* Arg(int index) const {
		if (index < 0 || static_cast<std::size_t>(index) >= args_.size()) {
			return nullptr;
		}
		return args_[static_cast<std::size_t>(index)].c_str();
	}

private:
	std::string text_;
	std::vector<std::string> args_;
};

// s2sdk hands the callback the engine's already-tokenised arguments (quotes stripped). A token
// that contained whitespace has to be quoted again, otherwise "mm_rename 3 \"John Doe\"" would
// reach the handler as "John" and the original's raw-string rename would lose the rest of the
// name. A token with a double quote in it is left alone (the engine's tokeniser would only
// corrupt it differently).
std::string QuoteToken(const plg::string& value) {
	const std::string_view token(value.data(), value.size());
	bool bNeedsQuotes = token.empty();
	for (const char c : token) {
		if (std::isspace(static_cast<unsigned char>(c))) {
			bNeedsQuotes = true;
			break;
		}
	}
	if (!bNeedsQuotes || token.find('"') != std::string_view::npos) {
		return std::string(token);
	}
	std::string out;
	out.reserve(token.size() + 2);
	out.push_back('"');
	out.append(token);
	out.push_back('"');
	return out;
}

// The command content as the original saw it: the registered spelling followed by the
// argument string (Pisex's CCommand::ArgS()).
std::string BuildCommandContent(const char* szName, const plg::vector<plg::string>& args) {
	std::string out(Safe(szName));
	for (std::size_t i = 1; i < args.size(); ++i) {
		out.push_back(' ');
		out += QuoteToken(args[i]);
	}
	return out;
}

// The original's "arguments after the command name" helper (raw text, not tokenised).
std::string ArgsAfterCommand(const char* szContent) {
	const std::string sContent(Safe(szContent));
	const std::size_t iPos = sContent.find(' ');
	return iPos == std::string::npos ? std::string{} : sContent.substr(iPos + 1);
}

bool ContainsOnlyDigits(const std::string& str) {
	return !str.empty() && str.find_first_not_of("0123456789") == std::string::npos;
}

// strcasestr() of the original.
bool NameContains(const char* szName, const std::string& szIdentity) {
	if (szName == nullptr || szIdentity.empty()) {
		return false;
	}
	const std::string sName(szName);
	const auto iPos = std::search(sName.begin(), sName.end(), szIdentity.begin(), szIdentity.end(),
								  [](char a, char b) {
									  return std::tolower(static_cast<unsigned char>(a)) ==
											 std::tolower(static_cast<unsigned char>(b));
								  });
	return iPos != sName.end();
}

// The original's FindUser(): a case-insensitive substring of the name, the full steam id or the
// slot number - tried for every slot with a controller and a non-zero SteamID64. Bots and
// unauthenticated clients have no id and stay untargetable by name, exactly like the original
// (whose GetSteamID64 answered 0 for them).
int32_t FindUser(const char* szIdentity) {
	if (szIdentity == nullptr || *szIdentity == '\0') {
		return -1;
	}
	const std::string sIdentity(szIdentity);
	const bool bDigits = ContainsOnlyDigits(sIdentity);
	long long iValue = 0;
	if (bDigits) {
		try {
			iValue = std::stoll(sIdentity);
		} catch (const std::exception&) {
			return -1;
		}
	}
	for (int32_t i = 0; i < kSlots; ++i) {
		if (!ControllerValid(i)) {
			continue;
		}
		const uint64_t iSteamID = ClientSteamID64(i);
		if (iSteamID == 0) {
			continue;
		}
		if (NameContains(ClientName(i).c_str(), sIdentity)) {
			return i;
		}
		if (bDigits && (static_cast<long long>(iSteamID) == iValue || iValue == i)) {
			return i;
		}
	}
	return -1;
}

// A menu selection that named a slot long ago: the slot still holds the same connection only
// when both the SteamID64 and the controller handle (entity serial = connection generation)
// match. A bot has no id, so it is accepted only while the slot still holds the same bot. -1 =
// do not act (PITFALLS C2/B4).
int32_t ResolveTarget(int32_t iSlot, uint64_t iSteamID64, int32_t iController) {
	if (!HasClient(iSlot) || iController == 0 || iController == -1 || !PawnValid(iSlot)) {
		// The menu item of a player who has no pawn any more (dead/spectating) is not the entry
		// the admin clicked - the original re-checked the controller *and* its pawn before it
		// ran the action of a selection.
		return -1;
	}
	if (ControllerHandle(iSlot) != iController) {
		return -1; // the slot now holds a different connection
	}
	if (iSteamID64 != 0) {
		if (IsBot(iSlot)) {
			return -1;
		}
		return ClientSteamID64(iSlot) == iSteamID64 ? iSlot : -1;
	}
	return IsBot(iSlot) ? iSlot : -1;
}

// ---------------------------------------------------------------------------------------------
// The actions (port of the original's free functions).
// ---------------------------------------------------------------------------------------------
void SendAction(int32_t iSlot, const char* szAction, const std::string& szParam) {
	if (g_pAdminApi == nullptr) {
		return;
	}
	g_pAdminApi->SendAction(iSlot, szAction, szParam.c_str());
}

int GetMessageType() {
	return g_pAdminApi != nullptr ? g_pAdminApi->GetMessageType() : 0;
}

void KickPlayer(int32_t iSlot, int32_t iTarget) {
	if (!HasClient(iTarget) || !__s2sdk_KickClient) {
		return;
	}
	try {
		s2sdk::KickClient(iTarget, s2sdk::NetworkDisconnectionReason::Kicked, plg::string());
	} catch (const std::exception& ex) {
		LogError(std::string("could not kick slot ") + std::to_string(iTarget) + ": " + ex.what());
		return;
	} catch (...) {
		LogError(std::string("could not kick slot ") + std::to_string(iTarget));
		return;
	}
	const int iMessageType = GetMessageType();
	if (iMessageType == 2) {
		PrintChatAllPhrase("AdminKickMessage", ActorName(iSlot).c_str(), ClientName(iTarget).c_str());
	} else if (iMessageType == 1) {
		PrintChatAllPhrase("KickMessage", ClientName(iTarget).c_str());
	}
	SendAction(iSlot, "kick", std::to_string(iTarget));
}

void KillPlayer(int32_t iSlot, int32_t iTarget) {
	if (!PawnValid(iTarget) || !__s2sdk_ForcePlayerSuicide) {
		return; // the original required the controller's pawn
	}
	try {
		s2sdk::ForcePlayerSuicide(iTarget, false, true);
	} catch (const std::exception& ex) {
		LogError(std::string("could not kill slot ") + std::to_string(iTarget) + ": " + ex.what());
		return;
	} catch (...) {
		LogError(std::string("could not kill slot ") + std::to_string(iTarget));
		return;
	}
	const int iMessageType = GetMessageType();
	if (iMessageType == 2) {
		PrintChatAllPhrase("AdminKillMessage", ActorName(iSlot).c_str(), ClientName(iTarget).c_str());
	} else if (iMessageType == 1) {
		PrintChatAllPhrase("KillMessage", ClientName(iTarget).c_str());
	}
	SendAction(iSlot, "kill", std::to_string(iTarget));
}

// SetClientName dereferences both the client and its controller without any check (PITFALLS
// C2), so the controller guard is mandatory here.
void ChangePlayerName(int32_t iSlot, int32_t iTarget, const char* szName) {
	if (!ControllerValid(iTarget) || !__s2sdk_SetClientName) {
		return;
	}
	const std::string sOldName = ClientName(iTarget);
	const int iMessageType = GetMessageType();
	if (iMessageType == 2) {
		PrintChatAllPhrase("AdminRenameMessage", ActorName(iSlot).c_str(), sOldName.c_str(),
						   Safe(szName));
	} else if (iMessageType == 1) {
		PrintChatAllPhrase("RenameMessage", sOldName.c_str(), Safe(szName));
	}
	try {
		s2sdk::SetClientName(iTarget, Plg(Safe(szName)));
	} catch (const std::exception& ex) {
		LogError(std::string("could not rename slot ") + std::to_string(iTarget) + ": " + ex.what());
		return;
	} catch (...) {
		LogError(std::string("could not rename slot ") + std::to_string(iTarget));
		return;
	}
	SendAction(iSlot, "rename", std::to_string(iTarget) + " " + Safe(szName));
}

void NoclipPlayer(int32_t iSlot, int32_t iTarget) {
	if (!PawnValid(iTarget) || !__s2sdk_GetClientMoveType || !__s2sdk_SetClientMoveType) {
		return;
	}
	bool bWasNoclip = false;
	if (!ClientNoclip(iTarget, bWasNoclip)) {
		return;
	}
	const bool bWanted = !bWasNoclip;
	try {
		s2sdk::SetClientMoveType(iTarget, bWanted ? s2sdk::MoveType::Noclip : s2sdk::MoveType::Walk);
	} catch (const std::exception& ex) {
		LogError(std::string("could not toggle noclip for slot ") + std::to_string(iTarget) + ": " +
				 ex.what());
		return;
	} catch (...) {
		LogError(std::string("could not toggle noclip for slot ") + std::to_string(iTarget));
		return;
	}
	// The original re-read the pawn's move type for the message after setting it; if the value
	// cannot be re-read the intended one is reported.
	bool bNoclipNow = bWanted;
	ClientNoclip(iTarget, bNoclipNow);
	SendAction(iSlot, "noclip", std::to_string(iTarget));
	const int iMessageType = GetMessageType();
	if (iMessageType == 2) {
		PrintChatAllPhrase(bNoclipNow ? "AdminNoclipMessageEnable" : "AdminNoclipMessageDisable",
						   ActorName(iSlot).c_str(), ClientName(iTarget).c_str());
	} else if (iMessageType == 1) {
		PrintChatAllPhrase(bNoclipNow ? "NoclipMessageEnable" : "NoclipMessageDisable",
						   ClientName(iTarget).c_str());
	}
}

// The original's mm_who. It shows admin flags, so it only answers when the admin core is loaded
// and its asynchronous admin SELECT for that exact player has finished: a pending/failed load
// must not be reported as "no flags" (admin_system/API.md §4.1).
void WhoPlayer(int32_t iSlot, int32_t iTarget, bool bConsole) {
	if (!g_adminBindingsAvailable || g_pAdminApi == nullptr) {
		LogError("mm_who: admin_system is not loaded - no admin data to show");
		return;
	}
	if (!HasClient(iTarget)) {
		return;
	}
	const uint64_t iSteamID64 = ClientSteamID64(iTarget);
	if (iSteamID64 == 0) {
		// A bot or an unauthorised client has no admin record to read.
		LogInfo("mm_who: slot " + std::to_string(iTarget) +
				" has no SteamID64 (bot/unauthorised) - no admin data to show");
		return;
	}
	if (__admin_system_IsClientAdminDataReady == nullptr) {
		LogError("mm_who: the loaded admin_system has no IsClientAdminDataReady (API 1.1.0) - "
				 "refusing to guess the admin data");
		return;
	}
	if (!AdminDataReady(iTarget, iSteamID64)) {
		LogInfo("mm_who: the admin data of slot " + std::to_string(iTarget) +
				" is not loaded yet - try again in a moment");
		return;
	}
	const std::vector<std::string> vFlags = g_pAdminApi->GetAdminFlags(iTarget);
	if (!vFlags.empty()) {
		if (bConsole) {
			// The original's console print took only the phrase, not the name.
			PrintConsole(iSlot, T("Item_Who_Flags"));
		} else {
			PrintChatPhrase(iSlot, "Item_Who_Flags", ClientName(iTarget).c_str());
		}
		for (const std::string& flag : vFlags) {
			const char* szFlagName = g_pAdminApi->GetFlagName(flag.c_str());
			if (bConsole) {
				PrintConsole(iSlot, Safe(szFlagName));
			} else {
				// The flag name is translated data, not a format string: it may contain a '%'.
				PrintChatText(iSlot, as_players::Format("%s", Safe(szFlagName)));
			}
		}
	} else {
		if (bConsole) {
			PrintConsole(iSlot, T("Item_Who_NoFlags"));
		} else {
			PrintChatText(iSlot, as_players::Format("%s", T("Item_Who_NoFlags")));
		}
	}
	SendAction(iSlot, "who", std::to_string(iTarget));
}

void SlapPlayer(int32_t iSlot, int32_t iTarget, int32_t iDamage) {
	if (!PawnValid(iTarget) || !__s2sdk_GetClientAbsVelocity || !__s2sdk_SetClientAbsVelocity) {
		return; // the original required the controller's pawn
	}
	try {
		plg::vec3 velocity = s2sdk::GetClientAbsVelocity(iTarget);
		velocity.x += static_cast<float>((std::rand() % 180) + 50) *
					  (((std::rand() % 2) == 1) ? -1.0f : 1.0f);
		velocity.y += static_cast<float>((std::rand() % 180) + 50) *
					  (((std::rand() % 2) == 1) ? -1.0f : 1.0f);
		velocity.z += static_cast<float>(std::rand() % 200 + 100);
		s2sdk::SetClientAbsVelocity(iTarget, velocity);
	} catch (const std::exception& ex) {
		LogError(std::string("could not slap slot ") + std::to_string(iTarget) + ": " + ex.what());
		return;
	} catch (...) {
		LogError(std::string("could not slap slot ") + std::to_string(iTarget));
		return;
	}
	if (iDamage > 0 && __s2sdk_GetClientHealth) {
		const int32_t iHealth = s2sdk::GetClientHealth(iTarget);
		if (iDamage >= iHealth) {
			// A lethal slap kills the pawn: the original committed suicide instead.
			if (__s2sdk_ForcePlayerSuicide) {
				s2sdk::ForcePlayerSuicide(iTarget, false, true);
			}
		} else if (__s2sdk_SetClientHealth) {
			s2sdk::SetClientHealth(iTarget, iHealth - iDamage);
			// The original marked the health field as changed after the write. NetworkStateChanged
			// takes the entity handle, not the index (PITFALLS C1); the pawn was validated at the
			// top of this function, in the same tick.
			const int32_t iPawn = PawnIndex(iTarget);
			if (iPawn > 0 && __s2sdk_EntIndexToEntHandle && __s2sdk_NetworkStateChanged) {
				const int32_t iHandle = s2sdk::EntIndexToEntHandle(iPawn);
				// The pawn handle is a signed CEntityHandle too: only 0/-1 are sentinels.
				const bool bValidHandle = iHandle != 0 && iHandle != -1 &&
										  (!__s2sdk_IsValidEntHandle ||
										   s2sdk::IsValidEntHandle(iHandle));
				if (bValidHandle) {
					s2sdk::NetworkStateChanged(iHandle, plg::string("CBaseEntity"),
											   plg::string("m_iHealth"));
				}
			}
		}
	}
	SendAction(iSlot, "slap", std::to_string(iTarget) + " " + std::to_string(iDamage));
	const int iMessageType = GetMessageType();
	if (iMessageType == 2) {
		PrintChatAllPhrase("AdminSlapMessage", ActorName(iSlot).c_str(), ClientName(iTarget).c_str(),
						   std::to_string(iDamage).c_str());
	} else if (iMessageType == 1) {
		PrintChatAllPhrase("SlapMessage", ClientName(iTarget).c_str(),
						   std::to_string(iDamage).c_str());
	}
}

void ChangePlayerTeam(int32_t iSlot, int32_t iTarget, int32_t iTeam, bool bSwitch) {
	if (iTeam < 0 || iTeam > 3 || !HasClient(iTarget)) {
		return;
	}
	try {
		if (bSwitch) {
			if (!__s2sdk_SwitchClientTeam) {
				return;
			}
			s2sdk::SwitchClientTeam(iTarget, static_cast<s2sdk::CSTeam>(iTeam));
		} else {
			if (!__s2sdk_ChangeClientTeam) {
				return;
			}
			s2sdk::ChangeClientTeam(iTarget, static_cast<s2sdk::CSTeam>(iTeam));
		}
	} catch (const std::exception& ex) {
		LogError(std::string("could not change the team of slot ") + std::to_string(iTarget) + ": " +
				 ex.what());
		return;
	} catch (...) {
		LogError(std::string("could not change the team of slot ") + std::to_string(iTarget));
		return;
	}
	SendAction(iSlot, "changeteam", std::to_string(iTarget) + " " + std::to_string(iTeam));
	constexpr const char* kTeams[] = {"none", "spectators", "terrorists", "counter-terrorists"};
	const int iMessageType = GetMessageType();
	if (iMessageType == 2) {
		PrintChatAllPhrase(bSwitch ? "AdminSwitchTeamMessage" : "AdminChangeTeamMessage",
						   ActorName(iSlot).c_str(), ClientName(iTarget).c_str(), T(kTeams[iTeam]));
	} else if (iMessageType == 1) {
		PrintChatAllPhrase(bSwitch ? "SwitchTeamMessage" : "ChangeTeamMessage",
						   ClientName(iTarget).c_str(), T(kTeams[iTeam]));
	}
}

std::string RandomName() {
	std::string sRandomName;
	for (int i = 0; i < kRandomNameLength; ++i) {
		const int iRandom = std::rand() % 52;
		sRandomName += iRandom < 26 ? static_cast<char>('a' + iRandom)
									: static_cast<char>('A' + (iRandom - 26));
	}
	return sRandomName;
}

// ---------------------------------------------------------------------------------------------
// The admin menu items (the original's RegisterCategory/RegisterItem block).
// ---------------------------------------------------------------------------------------------
struct ItemDef {
	const char* szIdentity;
	const char* szPhrase;
	const char* szPermission;
};

// Registry order of the original's AllPluginsLoaded block.
constexpr std::array<ItemDef, 8> kItems = {{{"who", "Item_Who", "@admin/who"},
											{"noclip", "Item_Noclip", "@admin/noclip"},
											{"switchteam", "Item_SwitchTeam", "@admin/switchteam"},
											{"changeteam", "Item_ChangeTeam", "@admin/changeteam"},
											{"rename", "Item_Rename", "@admin/rename"},
											{"slap", "Item_Slap", "@admin/slap"},
											{"kick", "Item_Kick", "@admin/kick"},
											{"kill", "Item_Kill", "@admin/kill"}}};

const ItemDef* FindItem(const char* szIdentity) {
	if (szIdentity == nullptr) {
		return nullptr;
	}
	const std::string_view identity(szIdentity);
	for (const ItemDef& item : kItems) {
		if (identity == item.szIdentity) {
			return &item;
		}
	}
	return nullptr;
}

const ItemDef* FindItem(const std::string& identity) {
	return FindItem(identity.c_str());
}

// ---------------------------------------------------------------------------------------------
// PITFALLS C10: the menu layer.
//
// One menu per player slot. A selection never performs an engine action inside the s2sdk menu
// handler (the handler runs under the menu manager's mutex): it stores a Pending action that
// OnPluginUpdate applies on the next frame, outside every s2sdk lock. The menu itself is kept
// open after a selection (SetMenuCloseOnSelect(false), like the Pisex menus); the update
// replaces it with the next menu, and every menu whose session has ended is destroyed through
// the deferred queue - s2sdk never destroys a menu on its own (PITFALLS C10).
// ---------------------------------------------------------------------------------------------
enum class MenuKind { None, Player, Team, Damage };

struct MenuItemSpec {
	std::string info;
	std::string display;
	s2sdk::MenuItemStyle style = s2sdk::MenuItemStyle::Default;
};

struct OpenMenu {
	s2sdk::MenuId id = 0;
	MenuKind kind = MenuKind::None;
	std::string lastItem;		   // the admin item the menu belongs to
	uint64_t ownerSteamID64 = 0;   // the admin the menu was shown to (slot-reuse guard)
	int32_t ownerController = -1;  // and the controller generation of that connection
	int32_t targetSlot = -1;	   // the slot the menu acts on (team/damage pickers)
	uint64_t targetSteamID64 = 0;
	int32_t targetController = -1;
	std::vector<std::string> values; // item info by absolute index (aligned with the added items)
};

enum class PendingKind {
	None,
	AdminItem,		 // a selection of an item of the "players" category -> open the player list
	PlayerItem,		 // a selection inside the player list -> run the action
	Team,			 // a selection inside the team picker -> change/switch the team
	Damage,			 // a selection inside the damage picker -> slap
	OpenListForItem, // "back" of a picker -> the player list of the same admin item
	AdminCategory,	 // "back" of the player list -> the admin core's last category
};

struct Pending {
	PendingKind kind = PendingKind::None;
	std::string lastItem;
	uint64_t actorSteamID64 = 0; // the admin that made the selection (slot-reuse guard)
	int32_t actorController = -1; // and the controller generation of that connection
	int32_t targetSlot = -1;
	uint64_t targetSteamID64 = 0;
	int32_t targetController = -1;
	int32_t number = 0;
};

std::array<OpenMenu, kSlots> g_openMenus;
std::array<Pending, kSlots> g_pending;

struct DestroyRequest {
	s2sdk::MenuId id = 0;
	int32_t slot = -1;
};

std::vector<DestroyRequest> g_destroyQueue;

// Defined with the rest of the menu layer below; forward declared because the menu builder and
// CloseSlotMenu refer to each other.
void OnMenuAction(s2sdk::MenuId id, s2sdk::MenuAction action, int32_t iSlot, int32_t iParam);
bool DisplayMenu(int32_t iSlot, MenuKind kind, const char* szTitleKey,
				 std::vector<MenuItemSpec> items, const std::string& lastItem, int32_t iTarget,
				 uint64_t iSteamID64);

// "slot:steamid:controllerHandle" - the identity a player list entry carries. The controller
// handle includes s2sdk's serial (generation), so a new connection in the same slot - even with
// the same SteamID64 - is not the entry the admin clicked. CEntityHandle is signed: the handle is
// written as a signed decimal and parsed back through a safe int64 range (INT32_MIN..INT32_MAX);
// negative handles are valid, only 0 and -1 are sentinels.
std::string EncodeTarget(int32_t iSlot, uint64_t iSteamID64, int32_t iController) {
	return std::to_string(iSlot) + ":" + std::to_string(iSteamID64) + ":" +
		   std::to_string(iController);
}

bool DecodeTarget(const std::string& value, int32_t& iSlotOut, uint64_t& iSteamID64Out,
				  int32_t& iControllerOut) {
	const std::size_t iFirst = value.find(':');
	const std::size_t iSecond = iFirst == std::string::npos ? std::string::npos
														   : value.find(':', iFirst + 1);
	if (iFirst == std::string::npos || iSecond == std::string::npos) {
		return false;
	}
	try {
		const long long iSlot = std::stoll(value.substr(0, iFirst));
		const unsigned long long iSteamID =
			std::stoull(value.substr(iFirst + 1, iSecond - iFirst - 1));
		// The serial is stored as a signed decimal CEntityHandle: parse it as int64 and reject
		// only values outside the int32 range (and the two sentinels). Negative handles are valid.
		const long long iController = std::stoll(value.substr(iSecond + 1));
		if (iSlot < 0 || iSlot >= kSlots || iController < INT32_MIN ||
			iController > INT32_MAX || iController == 0 || iController == -1) {
			return false;
		}
		iSlotOut = static_cast<int32_t>(iSlot);
		iSteamID64Out = static_cast<uint64_t>(iSteamID);
		iControllerOut = static_cast<int32_t>(iController);
		return true;
	} catch (const std::exception&) {
		return false;
	}
}

// The menu type of the new menu, with exactly the table pisexcompat used for every Pisex menu
// (libs/pisexcompat/src/menu.cpp ResolveS2sdkMenuTypeName/MappedMenuTypeName):
//
//   1. the player's own "!menu" choice from the hudmenu plugin (the original's cs2-menus
//      MenuType, persisted in the permissions cookie "Utils.MenuType") - an optional plugin API
//      dependency, not a wrapper library; "hud"/"wasd" are only used while the matching backend
//      is registered, otherwise "button" (even when the other custom backend is registered);
//      an unknown/foreign answer falls through to the mapped type;
//   2. otherwise the slot's Pisex MenuType. as_players never calls SetMenuType/SetDefaultMenuType
//      (the original module did not either), so this is pisexcompat's default HUD_LAYOUT:
//      "hud" when the backend is registered, otherwise "button" - NOT "centerhtml" (that is
//      Pisex's CENTER type, which this module never uses).
enum class PisexMenuType : int32_t {
	Chat = 0,
	Center = 1,
	CenterWasd = 2,
	HudLayout = 3,
};

bool IsMenuTypeRegisteredSafe(const char* szName) {
	if (__s2sdk_IsMenuTypeRegistered == nullptr || szName == nullptr) {
		return false; // pisexcompat's IsHudTypeRegistered/IsWasdTypeRegistered answer false too
	}
	try {
		return s2sdk::IsMenuTypeRegistered(plg::string(szName));
	} catch (...) {
		return false;
	}
}

// Maps a Pisex MenuType onto an s2sdk type name (used when no player choice applies).
std::string MappedMenuTypeName(PisexMenuType type, bool bHudRegistered, bool bWasdRegistered) {
	switch (type) {
	case PisexMenuType::Chat:
		return "chat";
	case PisexMenuType::Center:
		return "centerhtml";
	// Pisex's CENTER_WASD: the hudmenu "wasd" backend; s2sdk's built-in "button" is the fallback
	// while it is not registered.
	case PisexMenuType::CenterWasd:
		return bWasdRegistered ? "wasd" : "button";
	case PisexMenuType::HudLayout:
		return bHudRegistered ? "hud" : "button"; // without the backend, "button" is the closest
	}
	return "chat";
}

// The exact pisexcompat resolution: the hudmenu answer first, then the slot's Pisex MenuType.
// Only the known builtin names are taken from the player choice; anything else is ignored and
// the mapped type is used.
std::string ResolveMenuTypeName(std::string_view playerTypeChoice, PisexMenuType slotType,
								bool bHudRegistered, bool bWasdRegistered) {
	if (!playerTypeChoice.empty()) {
		if (playerTypeChoice == "chat" || playerTypeChoice == "centerhtml" ||
			playerTypeChoice == "button") {
			return std::string(playerTypeChoice);
		}
		if (playerTypeChoice == "hud") {
			return bHudRegistered ? "hud" : "button";
		}
		if (playerTypeChoice == "wasd") {
			return bWasdRegistered ? "wasd" : "button";
		}
	}
	return MappedMenuTypeName(slotType, bHudRegistered, bWasdRegistered);
}

std::string ChooseMenuType(int32_t iSlot) {
	std::string playerChoice;
	if (__hudmenu_GetClientMenuType != nullptr && HasClient(iSlot)) {
		try {
			playerChoice = ToStd(hudmenu::GetClientMenuType(iSlot));
		} catch (...) {
			playerChoice.clear();
		}
	}
	// No forced per-slot type: as_players never calls SetMenuType/SetDefaultMenuType, so the
	// mapped fallback is always pisexcompat's HUD_LAYOUT default.
	return ResolveMenuTypeName(playerChoice, PisexMenuType::HudLayout,
							   IsMenuTypeRegisteredSafe("hud"), IsMenuTypeRegisteredSafe("wasd"));
}

// MenuId 0 is valid for the manager but also means "no menu" in its client state, so a menu
// created as id 0 could never be shown or tracked (PITFALLS C10). Burn it and create again.
//
// The three menu-handle exports are required *before* anything is created, including the burnt
// id 0: without DestroyMenu/GetClientMenu/CancelClientMenu a menu handle could never be shown,
// tracked or closed safely (the whole menu layer is disabled when one of them is missing).
s2sdk::MenuId CreateNativeMenu(int32_t iSlot, const std::string& title) {
	if (!__s2sdk_CreateMenu || !__s2sdk_DestroyMenu || !__s2sdk_GetClientMenu ||
		!__s2sdk_CancelClientMenu || !__s2sdk_SetMenuType || !__s2sdk_GetMenuType) {
		return 0;
	}
	const std::string typeName = ChooseMenuType(iSlot);
	s2sdk::MenuId id = s2sdk::CreateMenu(Plg(title), &OnMenuAction, Plg(typeName));
	if (id == 0) {
		s2sdk::DestroyMenu(0);
		id = s2sdk::CreateMenu(Plg(title), &OnMenuAction, Plg(typeName));
	}
	return id;
}

void ClearOpenMenu(int32_t iSlot) {
	g_openMenus[static_cast<std::size_t>(iSlot)] = OpenMenu{};
}

void QueueMenuDestroy(s2sdk::MenuId id, int32_t iSlot) {
	if (id != 0) {
		g_destroyQueue.push_back(DestroyRequest{id, iSlot});
	}
}

// --- safe menu destruction (PITFALLS B2 + A7) -------------------------------------------------
//
// s2sdk's menu manager runs GetClientMenu/CancelClientMenu/DestroyMenu on the game thread under
// one recursive mutex, so a display checked with GetClientMenu() cannot change between two calls
// of this plugin. CancelClientMenu(slot, Disconnect) ends the session without calling the backend
// close; it answers false only when the slot has no display any more. DestroyMenu(id) ends a
// still-active display with reason Destroyed and *does* call the backend close - which, between
// OnMapEnd and the next OnMapStart, reaches the client/entity layer being torn down (B2).
//
// The builtin "chat"/"console" closers are no-ops (menu_types_builtin.cpp). Therefore a menu that
// is still displayed while no map is up is switched to one of those types (verified through
// GetMenuType) before DestroyMenu: the close becomes a no-op and the destroy is safe. If even
// that switch is impossible, the request is parked (retried when a map is back) and reported as
// A7_UNRESOLVED - the plugin never pretends a menu it could not release was cleaned up.

std::vector<s2sdk::MenuId> g_unresolvedMenus;

void ReportUnresolvedMenu(s2sdk::MenuId id, int32_t iSlot, bool bForce) {
	if (!bForce) {
		if (std::find(g_unresolvedMenus.begin(), g_unresolvedMenus.end(), id) !=
			g_unresolvedMenus.end()) {
			return;
		}
		g_unresolvedMenus.push_back(id);
	}
	LogError("A7_UNRESOLVED: menu " + std::to_string(id) + " on slot " +
			 std::to_string(iSlot) +
			 " is still displayed and cannot be switched to a no-op-close type; the handle (and "
			 "this module's handler pointer) could not be released");
}

// Replaces the menu type with a builtin whose close() is a no-op, so a DestroyMenu that still has
// to end a display cannot reach the client layer.
bool MakeMenuCloseNoOp(s2sdk::MenuId id) {
	if (__s2sdk_SetMenuType == nullptr || __s2sdk_GetMenuType == nullptr) {
		return false;
	}
	for (const char* szType : {"chat", "console"}) {
		if (__s2sdk_IsMenuTypeRegistered != nullptr) {
			bool bRegistered = false;
			try {
				bRegistered = s2sdk::IsMenuTypeRegistered(plg::string(szType));
			} catch (...) {
				bRegistered = false;
			}
			if (!bRegistered) {
				continue;
			}
		}
		try {
			if (!s2sdk::SetMenuType(id, plg::string(szType))) {
				continue;
			}
			if (ToStd(s2sdk::GetMenuType(id)) == szType) {
				return true;
			}
		} catch (...) {
		}
	}
	return false;
}

// Ends every display of `id` and destroys the menu. mapReady == false is the changelevel window,
// where a backend close must not touch a client. Returns true when the handle is gone.
bool DestroyMenuChecked(s2sdk::MenuId id, int32_t iSlot, bool bMapReady) {
	if (__s2sdk_DestroyMenu == nullptr) {
		return false;
	}
	if (id == 0) {
		// Client state 0 is s2sdk's "no menu": id 0 can never be displayed, so a plain destroy is
		// safe (it only frees the burnt handle).
		try {
			s2sdk::DestroyMenu(0);
		} catch (...) {
			return false;
		}
		return true;
	}
	if (__s2sdk_IsValidMenu != nullptr) {
		bool bValid = true;
		try {
			bValid = s2sdk::IsValidMenu(id);
		} catch (...) {
			bValid = true;
		}
		if (!bValid) {
			return true; // already gone
		}
	}
	// Find every slot that still displays the menu (the manager allows one menu on several
	// clients; this plugin shows one per slot, but the cleanup must not depend on its own
	// bookkeeping being right).
	auto collectDisplays = [id]() {
		std::vector<int32_t> slots;
		if (__s2sdk_GetClientMenu == nullptr) {
			return slots;
		}
		for (int32_t slot = 0; slot < kSlots; ++slot) {
			try {
				if (s2sdk::GetClientMenu(slot) == id) {
					slots.push_back(slot);
				}
			} catch (...) {
			}
		}
		return slots;
	};
	std::vector<int32_t> displayed = collectDisplays();
	if (!displayed.empty()) {
		if (__s2sdk_CancelClientMenu != nullptr) {
			for (const int32_t slot : displayed) {
				try {
					(void)s2sdk::CancelClientMenu(slot, s2sdk::MenuCancelReason::Disconnect);
				} catch (...) {
				}
			}
		}
		// Re-query whether the cancel answered true or false: only the manager's own state
		// decides, and a display that survived must not be destroyed with a backend close in a
		// map-down window.
		displayed = collectDisplays();
		if (!displayed.empty() && !bMapReady) {
			if (!MakeMenuCloseNoOp(id)) {
				ReportUnresolvedMenu(id, displayed.front(), false);
				return false; // parked: a direct DestroyMenu would reach the torn-down client
			}
		}
	}
	try {
		s2sdk::DestroyMenu(id);
	} catch (...) {
		ReportUnresolvedMenu(id, iSlot, false);
		return false;
	}
	if (__s2sdk_IsValidMenu != nullptr) {
		bool bStillValid = false;
		try {
			bStillValid = s2sdk::IsValidMenu(id);
		} catch (...) {
			bStillValid = false;
		}
		if (bStillValid) {
			ReportUnresolvedMenu(id, iSlot, false);
			return false;
		}
	}
	g_unresolvedMenus.erase(
		std::remove(g_unresolvedMenus.begin(), g_unresolvedMenus.end(), id),
		g_unresolvedMenus.end());
	return true;
}

// Ends a still-displayed menu of the slot right now (the caller is outside every s2sdk handler:
// OnPluginUpdate / ApplyPending / OnPluginEnd). Cancel is used so the backend closes its UI; the
// manager's own End then reaches this plugin's handler, which only queues the destroy.
void CloseSlotMenu(int32_t iSlot) {
	OpenMenu& menu = g_openMenus[static_cast<std::size_t>(iSlot)];
	if (menu.id == 0) {
		return;
	}
	const s2sdk::MenuId id = menu.id;
	if (DestroyMenuChecked(id, iSlot, g_mapReady)) {
		ClearOpenMenu(iSlot);
	}
	// If the handle had to be parked (only possible outside a map), the state is kept so the next
	// cleanup pass - and OnPluginEnd - can try again.
}

// Destroys every menu whose session has ended. A menu the manager still shows for its slot
// (which should not happen) is cancelled first - with Disconnect while no map is up, so the
// backend never touches a torn-down client (PITFALLS B2).
void DrainDestroyQueue() {
	if (g_destroyQueue.empty()) {
		return;
	}
	std::vector<DestroyRequest> queue = std::move(g_destroyQueue);
	g_destroyQueue.clear();
	for (const DestroyRequest& request : queue) {
		if (!DestroyMenuChecked(request.id, request.slot, g_mapReady)) {
			// Only a map-down display that could not be switched to a no-op backend gets here:
			// keep the request for the next update / the next map instead of a blind DestroyMenu.
			g_destroyQueue.push_back(request);
			continue;
		}
		if (request.slot >= 0 && request.slot < kSlots &&
			g_openMenus[static_cast<std::size_t>(request.slot)].id == request.id) {
			ClearOpenMenu(request.slot);
		}
	}
}

// OnMapEnd: drop the bookkeeping only. No menu-manager or client call is made from here; the
// queued menus are destroyed by OnPluginUpdate, which runs outside the map teardown
// (PITFALLS B2).
void DropMenusForMapEnd() {
	for (int32_t i = 0; i < kSlots; ++i) {
		const s2sdk::MenuId id = g_openMenus[static_cast<std::size_t>(i)].id;
		if (id != 0) {
			// End the display session through the menu manager only: Disconnect skips the backend
			// close, so neither the HUD backend nor a client is touched while the map is being
			// torn down (PITFALLS B2). The handle itself is destroyed on the next update; if the
			// cancel is refused and the display survives, DestroyMenuChecked() re-queries it and
			// switches the menu to a no-op-close type before the destroy (A7).
			if (__s2sdk_GetClientMenu != nullptr && __s2sdk_CancelClientMenu != nullptr &&
				s2sdk::GetClientMenu(i) == id) {
				s2sdk::CancelClientMenu(i, s2sdk::MenuCancelReason::Disconnect);
			}
			QueueMenuDestroy(id, i);
			ClearOpenMenu(i);
		}
		g_pending[static_cast<std::size_t>(i)] = Pending{};
	}
}

bool HasPermissionForIdentity(int32_t iSlot, const ItemDef* item) {
	// Same canonical, current-SteamID gate as the commands: a selection that outlived its menu
	// must not run on a stale record of the slot (API.md §4.1).
	return item != nullptr && HasReadyPermission(iSlot, item->szPermission);
}

// Shows the player picker of one admin item. Only slots whose name/identity can really be read
// are offered (controller + pawn + a SteamID64, or a bot), so no entry points at data the admin
// could not act on (PITFALLS C2).
bool ShowPlayerMenu(int32_t iSlot, const std::string& identity) {
	if (identity.empty()) {
		return false;
	}
	const bool bAliveOnly = identity == "kill" || identity == "slap" || identity == "noclip";
	std::vector<MenuItemSpec> items;
	items.reserve(kSlots);
	for (int32_t i = 0; i < kSlots; ++i) {
		if (!PawnValid(i)) {
			continue; // the original required the controller and its pawn for every item
		}
		const bool bBot = IsBot(i);
		if (identity == "who" && bBot) {
			continue; // no SteamID64, no admin record: the who item could show nothing
		}
		uint64_t iSteamID64 = 0;
		if (!bBot) {
			iSteamID64 = ClientSteamID64(i);
			if (iSteamID64 == 0) {
				continue; // no identity: the selection could hit the next occupant of the slot
			}
		}
		if (bAliveOnly && !ClientAlive(i)) {
			continue;
		}
		MenuItemSpec item;
		item.info = EncodeTarget(i, iSteamID64, ControllerHandle(i));
		std::string display = ClientName(i);
		if (identity == "noclip") {
			bool bNoclip = false;
			if (ClientNoclip(i, bNoclip)) {
				display += bNoclip ? " [ON]" : " [OFF]";
			}
		}
		item.display = std::move(display);
		items.push_back(std::move(item));
	}
	if (items.empty()) {
		MenuItemSpec item;
		item.display = T("NoPlayers");
		item.style = s2sdk::MenuItemStyle::Disabled;
		items.push_back(std::move(item));
	}
	return DisplayMenu(iSlot, MenuKind::Player, "SelectPlayer", std::move(items), identity, -1, 0);
}

bool ShowTeamMenu(int32_t iSlot, int32_t iTarget, uint64_t iSteamID64,
				  const std::string& lastItem) {
	if (iTarget < 0 || iTarget >= kSlots || !ControllerValid(iTarget)) {
		return false;
	}
	std::vector<MenuItemSpec> items;
	items.push_back(MenuItemSpec{"1", "SPEC", s2sdk::MenuItemStyle::Default});
	items.push_back(MenuItemSpec{"2", "T", s2sdk::MenuItemStyle::Default});
	items.push_back(MenuItemSpec{"3", "CT", s2sdk::MenuItemStyle::Default});
	return DisplayMenu(iSlot, MenuKind::Team, "SelectTeam", std::move(items), lastItem, iTarget,
					   iSteamID64);
}

bool ShowDamageMenu(int32_t iSlot, int32_t iTarget, uint64_t iSteamID64,
					const std::string& lastItem) {
	if (iTarget < 0 || iTarget >= kSlots || !PawnValid(iTarget)) {
		return false;
	}
	std::vector<MenuItemSpec> items;
	for (const char* szDamage : {"0", "1", "5", "10", "20", "50", "100"}) {
		items.push_back(MenuItemSpec{szDamage, szDamage, s2sdk::MenuItemStyle::Default});
	}
	return DisplayMenu(iSlot, MenuKind::Damage, "Item_Slap_Damage", std::move(items), lastItem,
					   iTarget, iSteamID64);
}

// The Pisex "DisplayPlayerMenu" of the ported module: five items per page, exit and back
// buttons, no auto-close on selection. Runs from OnPluginUpdate only.
bool DisplayMenu(int32_t iSlot, MenuKind kind, const char* szTitleKey,
				 std::vector<MenuItemSpec> items, const std::string& lastItem, int32_t iTarget,
				 uint64_t iSteamID64) {
	if (!g_menuBindingsAvailable || !g_mapReady || !HasClient(iSlot)) {
		return false;
	}
	const uint64_t ownerSteamID64 = ClientSteamID64(iSlot);
	if (ownerSteamID64 == 0) {
		// A menu outlives the frame of its selection; without an identity a reused slot could
		// inherit the next player's selection.
		LogInfo("not showing a menu on slot " + std::to_string(iSlot) +
				": the client has no SteamID64 yet");
		return false;
	}
	const int32_t ownerController = ControllerHandle(iSlot);
	if (ownerController == -1) {
		LogInfo("not showing a menu on slot " + std::to_string(iSlot) +
				": the controller is not valid");
		return false;
	}
	CloseSlotMenu(iSlot);
	const s2sdk::MenuId id = CreateNativeMenu(iSlot, T(szTitleKey));
	if (id == 0) {
		LogError("s2sdk refused to create a menu");
		return false;
	}
	OpenMenu state;
	state.kind = kind;
	state.lastItem = lastItem;
	state.ownerSteamID64 = ownerSteamID64;
	state.ownerController = ownerController;
	state.targetSlot = iTarget;
	state.targetSteamID64 = iSteamID64;
	state.targetController = iTarget >= 0 && iTarget < kSlots ? ControllerHandle(iTarget) : -1;
	s2sdk::SetMenuPagination(id, kItemsPerPage);
	s2sdk::SetMenuExitButton(id, true);
	s2sdk::SetMenuExitBackButton(id, true);
	s2sdk::SetMenuCloseOnSelect(id, false);
	for (const MenuItemSpec& item : items) {
		const int iIndex = s2sdk::AddMenuItem(id, Plg(item.info), Plg(item.display), item.style);
		if (iIndex >= 0) {
			state.values.push_back(item.info);
		}
	}
	state.id = id;
	g_openMenus[static_cast<std::size_t>(iSlot)] = std::move(state);
	if (!s2sdk::DisplayMenu(id, iSlot, kMenuTimeoutSeconds)) {
		ClearOpenMenu(iSlot);
		s2sdk::DestroyMenu(id);
		LogError("s2sdk could not display a menu on slot " + std::to_string(iSlot));
		return false;
	}
	return true;
}

void OpenAdminCategoryMenu(int32_t iSlot) {
	if (g_pAdminApi != nullptr) {
		g_pAdminApi->ShowAdminLastCategoryMenu(iSlot);
	}
}

void HandleMenuSelect(int32_t iSlot, const OpenMenu& menu, int32_t iParam) {
	if (iParam < 0 || static_cast<std::size_t>(iParam) >= menu.values.size()) {
		return;
	}
	if (menu.ownerSteamID64 == 0 || menu.ownerController == 0 || menu.ownerController == -1 ||
		ClientSteamID64(iSlot) != menu.ownerSteamID64 ||
		ControllerHandle(iSlot) != menu.ownerController) {
		return; // the slot's owner (or its connection generation) changed: not their selection
	}
	// s2sdk maps digits 1..7 to currentOffset + digit - 1 even when the page shows five items,
	// so a digit that belongs to the next (not displayed) page must be ignored, exactly like
	// pisexcompat's guard did.
	int32_t iOffset = iParam - (iParam % kItemsPerPage);
	if (__s2sdk_GetClientMenuOffset != nullptr) {
		iOffset = s2sdk::GetClientMenuOffset(iSlot);
	}
	if (iParam < iOffset || iParam >= iOffset + kItemsPerPage) {
		return;
	}
	const std::string& value = menu.values[static_cast<std::size_t>(iParam)];
	Pending pending;
	pending.lastItem = menu.lastItem;
	pending.actorSteamID64 = menu.ownerSteamID64;
	pending.actorController = menu.ownerController;
	pending.targetSlot = menu.targetSlot;
	pending.targetSteamID64 = menu.targetSteamID64;
	pending.targetController = menu.targetController;
	switch (menu.kind) {
	case MenuKind::Player: {
		int32_t iTarget = -1;
		uint64_t iSteamID64 = 0;
		int32_t iController = -1;
		if (!DecodeTarget(value, iTarget, iSteamID64, iController)) {
			return;
		}
		pending.kind = PendingKind::PlayerItem;
		pending.targetSlot = iTarget;
		pending.targetSteamID64 = iSteamID64;
		pending.targetController = iController;
		break;
	}
	case MenuKind::Team:
	case MenuKind::Damage: {
		try {
			pending.number = std::stoi(value);
		} catch (const std::exception&) {
			return;
		}
		pending.kind = menu.kind == MenuKind::Team ? PendingKind::Team : PendingKind::Damage;
		break;
	}
	case MenuKind::None:
		return;
	}
	g_pending[static_cast<std::size_t>(iSlot)] = std::move(pending);
}

// The s2sdk menu handler. It runs under the menu manager's mutex and must not perform engine
// actions; it only records state and a Pending action for OnPluginUpdate (PITFALLS C10).
void OnMenuAction(s2sdk::MenuId id, s2sdk::MenuAction action, int32_t iSlot, int32_t iParam) {
	try {
		if (iSlot < 0 || iSlot >= kSlots) {
			return;
		}
		OpenMenu& menu = g_openMenus[static_cast<std::size_t>(iSlot)];
		if (menu.id == 0 || menu.id != id) {
			return; // stale (the plugin already replaced or destroyed this menu)
		}
		switch (action) {
		case s2sdk::MenuAction::Start:
			return;
		case s2sdk::MenuAction::Select:
			HandleMenuSelect(iSlot, menu, iParam);
			return;
		case s2sdk::MenuAction::Cancel: {
			const auto reason = static_cast<s2sdk::MenuCancelReason>(iParam);
			const MenuKind kind = menu.kind;
			const std::string lastItem = menu.lastItem;
			const uint64_t ownerSteamID64 = menu.ownerSteamID64;
			const int32_t ownerController = menu.ownerController;
			QueueMenuDestroy(id, iSlot);
			ClearOpenMenu(iSlot);
			if (reason == s2sdk::MenuCancelReason::ExitBack) {
				Pending pending;
				pending.actorSteamID64 = ownerSteamID64;
				pending.actorController = ownerController;
				if (kind == MenuKind::Player) {
					pending.kind = PendingKind::AdminCategory;
				} else if (kind == MenuKind::Team || kind == MenuKind::Damage) {
					pending.kind = PendingKind::OpenListForItem;
					pending.lastItem = lastItem;
				} else {
					return;
				}
				g_pending[static_cast<std::size_t>(iSlot)] = std::move(pending);
			}
			return;
		}
		case s2sdk::MenuAction::End:
			QueueMenuDestroy(id, iSlot);
			ClearOpenMenu(iSlot);
			return;
		}
	} catch (const std::exception& ex) {
		LogError(std::string("menu handler failed: ") + ex.what());
	} catch (...) {
		LogError("menu handler failed with an unknown exception");
	}
}

// The action of one player-picker item. Runs outside the menu handler.
void ExecutePlayerItem(int32_t iSlot, const ItemDef* item, int32_t iTarget, uint64_t iSteamID64) {
	const std::string identity = item->szIdentity;
	if (identity == "kill") {
		KillPlayer(iSlot, iTarget);
		ShowPlayerMenu(iSlot, identity);
	} else if (identity == "kick") {
		KickPlayer(iSlot, iTarget);
		ShowPlayerMenu(iSlot, identity);
	} else if (identity == "noclip") {
		NoclipPlayer(iSlot, iTarget);
		ShowPlayerMenu(iSlot, identity);
	} else if (identity == "slap") {
		if (!ShowDamageMenu(iSlot, iTarget, iSteamID64, identity)) {
			ShowPlayerMenu(iSlot, identity);
		}
	} else if (identity == "rename") {
		ChangePlayerName(iSlot, iTarget, RandomName().c_str());
		ShowPlayerMenu(iSlot, identity);
	} else if (identity == "changeteam" || identity == "switchteam") {
		if (!ShowTeamMenu(iSlot, iTarget, iSteamID64, identity)) {
			ShowPlayerMenu(iSlot, identity);
		}
	} else if (identity == "who") {
		WhoPlayer(iSlot, iTarget, false);
		ShowPlayerMenu(iSlot, identity);
	}
}

// Applies one deferred selection, outside every s2sdk menu handler.
void ApplyPending(int32_t iSlot) {
	const Pending pending = g_pending[static_cast<std::size_t>(iSlot)];
	if (pending.kind == PendingKind::None) {
		return;
	}
	g_pending[static_cast<std::size_t>(iSlot)] = Pending{};
	if (!g_mapReady || !HasClient(iSlot)) {
		return; // the admin is gone; OnClientDisconnect already ended the menu session
	}
	if (pending.actorSteamID64 == 0 || pending.actorController == 0 ||
		pending.actorController == -1 ||
		ClientSteamID64(iSlot) != pending.actorSteamID64 ||
		ControllerHandle(iSlot) != pending.actorController) {
		// The slot was reused (or reconnected) after the selection: never run an admin action on
		// behalf of the new connection. The menu this plugin still tracks belongs to the old
		// occupant, so it is closed rather than left on the new player's screen.
		CloseSlotMenu(iSlot);
		return;
	}
	CloseSlotMenu(iSlot);
	switch (pending.kind) {
	case PendingKind::None:
		return;
	case PendingKind::AdminCategory:
		OpenAdminCategoryMenu(iSlot);
		return;
	case PendingKind::AdminItem:
	case PendingKind::OpenListForItem: {
		const ItemDef* item = FindItem(pending.lastItem);
		if (!HasPermissionForIdentity(iSlot, item)) {
			LogInfo("the permission for '" + pending.lastItem + "' is not ready/held for slot " +
					std::to_string(iSlot));
			return;
		}
		ShowPlayerMenu(iSlot, item->szIdentity);
		return;
	}
	case PendingKind::PlayerItem: {
		const ItemDef* item = FindItem(pending.lastItem);
		if (!HasPermissionForIdentity(iSlot, item)) {
			LogInfo("the permission for '" + pending.lastItem + "' is not ready/held for slot " +
					std::to_string(iSlot));
			return;
		}
		const int32_t iTarget =
			ResolveTarget(pending.targetSlot, pending.targetSteamID64, pending.targetController);
		if (iTarget < 0) {
			// The player left, reconnected or the slot was reused: never act on the new
			// connection (PITFALLS C2/B4). A fresh list shows what is possible now.
			ShowPlayerMenu(iSlot, item->szIdentity);
			return;
		}
		ExecutePlayerItem(iSlot, item, iTarget, pending.targetSteamID64);
		return;
	}
	case PendingKind::Team: {
		const ItemDef* item = FindItem(pending.lastItem);
		if (!HasPermissionForIdentity(iSlot, item)) {
			return;
		}
		const int32_t iTarget =
			ResolveTarget(pending.targetSlot, pending.targetSteamID64, pending.targetController);
		if (iTarget < 0) {
			ShowPlayerMenu(iSlot, item != nullptr ? item->szIdentity : std::string("changeteam"));
			return;
		}
		const bool bSwitch = item != nullptr && std::string_view(item->szIdentity) == "switchteam";
		ChangePlayerTeam(iSlot, iTarget, pending.number, bSwitch);
		if (!ShowTeamMenu(iSlot, iTarget, pending.targetSteamID64,
						  item != nullptr ? std::string(item->szIdentity) : std::string("changeteam"))) {
			ShowPlayerMenu(iSlot, item != nullptr ? item->szIdentity : std::string("changeteam"));
		}
		return;
	}
	case PendingKind::Damage: {
		const ItemDef* item = FindItem("slap");
		if (!HasPermissionForIdentity(iSlot, item)) {
			return;
		}
		const int32_t iTarget =
			ResolveTarget(pending.targetSlot, pending.targetSteamID64, pending.targetController);
		if (iTarget < 0) {
			ShowPlayerMenu(iSlot, "slap");
			return;
		}
		SlapPlayer(iSlot, iTarget, pending.number);
		if (!ShowDamageMenu(iSlot, iTarget, pending.targetSteamID64, "slap")) {
			ShowPlayerMenu(iSlot, "slap");
		}
		return;
	}
	}
}

// Called by the admin core when an item of the "players" category is selected. It runs inside
// the core's (pisexcompat) menu handler, so it only queues the next menu for OnPluginUpdate.
void OnAdminItemSelect(int32_t iSlot, const char* szCategory, const char* szIdentity,
					   const char* /*szItem*/) {
	try {
		if (iSlot < 0 || iSlot >= kSlots || szCategory == nullptr || szIdentity == nullptr) {
			return;
		}
		if (std::string_view(szCategory) != "players") {
			return;
		}
		const ItemDef* item = FindItem(szIdentity);
		if (item == nullptr) {
			return;
		}
		const uint64_t actorSteamID64 = ClientSteamID64(iSlot);
		const int32_t actorController = ControllerHandle(iSlot);
		if (actorSteamID64 == 0 || actorController == -1) {
			return; // no identity: a reused slot could otherwise inherit this request
		}
		Pending pending;
		pending.kind = PendingKind::AdminItem;
		pending.lastItem = item->szIdentity;
		pending.actorSteamID64 = actorSteamID64;
		pending.actorController = actorController;
		g_pending[static_cast<std::size_t>(iSlot)] = std::move(pending);
	} catch (const std::exception& ex) {
		LogError(std::string("admin item callback failed: ") + ex.what());
	} catch (...) {
		LogError("admin item callback failed with an unknown exception");
	}
}

// ---------------------------------------------------------------------------------------------
// The console commands (the original's CON_COMMAND_F bodies).
// ---------------------------------------------------------------------------------------------
bool HandleKillCommand(int32_t iSlot, const char* szContent);
bool HandleKickCommand(int32_t iSlot, const char* szContent);
bool HandleSlapCommand(int32_t iSlot, const char* szContent);
bool HandleRenameCommand(int32_t iSlot, const char* szContent);
bool HandleChangeTeamCommand(int32_t iSlot, const char* szContent);
bool HandleSwitchTeamCommand(int32_t iSlot, const char* szContent);
bool HandleNoclipCommand(int32_t iSlot, const char* szContent);
bool HandleWhoCommand(int32_t iSlot, const char* szContent);

s2sdk::ResultType OnKillCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
								const plg::vector<plg::string>& args);
s2sdk::ResultType OnKickCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
								const plg::vector<plg::string>& args);
s2sdk::ResultType OnSlapCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
								const plg::vector<plg::string>& args);
s2sdk::ResultType OnRenameCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
								  const plg::vector<plg::string>& args);
s2sdk::ResultType OnChangeTeamCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
									  const plg::vector<plg::string>& args);
s2sdk::ResultType OnSwitchTeamCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
									  const plg::vector<plg::string>& args);
s2sdk::ResultType OnNoclipCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
								  const plg::vector<plg::string>& args);
s2sdk::ResultType OnWhoCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
							   const plg::vector<plg::string>& args);

enum CommandIndex : std::size_t {
	kCmdKill = 0,
	kCmdKick,
	kCmdSlap,
	kCmdRename,
	kCmdChangeTeam,
	kCmdSwitchTeam,
	kCmdNoclip,
	kCmdWho,
	kCmdCount,
};

struct CommandDef {
	const char* szName;
	bool (*handler)(int32_t, const char*);
	s2sdk::ConCommandCallback callback;
	bool registered = false;
	bool created = false;
};

std::array<CommandDef, kCmdCount> g_commands = {{
	{"mm_kill", &HandleKillCommand, &OnKillCommand},
	{"mm_kick", &HandleKickCommand, &OnKickCommand},
	{"mm_slap", &HandleSlapCommand, &OnSlapCommand},
	{"mm_rename", &HandleRenameCommand, &OnRenameCommand},
	{"mm_changeteam", &HandleChangeTeamCommand, &OnChangeTeamCommand},
	{"mm_switchteam", &HandleSwitchTeamCommand, &OnSwitchTeamCommand},
	{"mm_noclip", &HandleNoclipCommand, &OnNoclipCommand},
	{"mm_who", &HandleWhoCommand, &OnWhoCommand},
}};

// The s2sdk callback: console commands only. A chat "!mm_kill" reaches the same callback with
// ConCommandContext::Chat; the ported commands never had a chat spelling, so the line is left
// to s2sdk's normal chat path (Continue), exactly like pisexcompat's chat-name filter did.
s2sdk::ResultType RunConsoleCommand(const CommandDef& def, int32_t iSlot,
									s2sdk::ConCommandContext eContext,
									const plg::vector<plg::string>& args) {
	if (eContext == s2sdk::ConCommandContext::Chat) {
		return s2sdk::ResultType::Continue;
	}
	try {
		const std::string content = BuildCommandContent(def.szName, args);
		if (def.handler != nullptr) {
			def.handler(iSlot, content.c_str());
		}
	} catch (const std::exception& ex) {
		LogError(std::string(def.szName) + " failed: " + ex.what());
	} catch (...) {
		LogError(std::string(def.szName) + " failed with an unknown exception");
	}
	// A handler that threw still owns the command (PITFALLS A: never unwind into the engine).
	return s2sdk::ResultType::Handled;
}

bool HandleKillCommand(int32_t iSlot, const char* szContent) {
	if (iSlot < -1 || iSlot >= kSlots) {
		return true; // invalid caller: fail closed, never treat it as the server console
	}
	const bool bConsole = iSlot == -1;
	if (!bConsole && !HasReadyPermission(iSlot, "@admin/kill")) {
		return true;
	}
	CommandArgs args;
	args.Tokenize(szContent);
	if (args.ArgC() < 2) {
		PrintUsage(iSlot, "UsageDefault", args.Arg(0));
		return true;
	}
	const int32_t iTarget = FindUser(args.Arg(1));
	if (iTarget < 0) {
		return true;
	}
	KillPlayer(iSlot, iTarget);
	return true;
}

bool HandleKickCommand(int32_t iSlot, const char* szContent) {
	if (iSlot < -1 || iSlot >= kSlots) {
		return true;
	}
	const bool bConsole = iSlot == -1;
	if (!bConsole && !HasReadyPermission(iSlot, "@admin/kick")) {
		return true;
	}
	CommandArgs args;
	args.Tokenize(szContent);
	if (args.ArgC() < 2) {
		PrintUsage(iSlot, "UsageDefault", args.Arg(0));
		return true;
	}
	const int32_t iTarget = FindUser(args.Arg(1));
	if (iTarget < 0) {
		return true;
	}
	KickPlayer(iSlot, iTarget);
	return true;
}

bool HandleSlapCommand(int32_t iSlot, const char* szContent) {
	if (iSlot < -1 || iSlot >= kSlots) {
		return true;
	}
	const bool bConsole = iSlot == -1;
	if (!bConsole && !HasReadyPermission(iSlot, "@admin/slap")) {
		return true;
	}
	CommandArgs args;
	args.Tokenize(szContent);
	if (args.ArgC() < 3) {
		PrintUsage(iSlot, "UsageSlap", args.Arg(0));
		return true;
	}
	const int32_t iTarget = FindUser(args.Arg(1));
	if (iTarget < 0) {
		return true;
	}
	const std::string sDamage = args.Arg(2) != nullptr ? args.Arg(2) : "";
	if (!sDamage.empty() &&
		(sDamage.length() > 3 || sDamage.find_first_not_of("0123456789") != std::string::npos)) {
		PrintUsage(iSlot, "UsageSlap", args.Arg(0));
		return true;
	}
	SlapPlayer(iSlot, iTarget, std::atoi(sDamage.c_str()));
	return true;
}

bool HandleRenameCommand(int32_t iSlot, const char* szContent) {
	if (iSlot < -1 || iSlot >= kSlots) {
		return true;
	}
	const bool bConsole = iSlot == -1;
	if (!bConsole && !HasReadyPermission(iSlot, "@admin/rename")) {
		return true;
	}
	CommandArgs args;
	args.Tokenize(szContent);
	if (args.ArgC() < 3) {
		PrintUsage(iSlot, "UsageRename", args.Arg(0));
		return true;
	}
	const int32_t iTarget = FindUser(args.Arg(1));
	if (iTarget < 0) {
		return true;
	}
	// The original took the raw argument string, dropped its first character and then the first
	// argument's length; the two erases together remove the target and the space after it.
	std::string sName = ArgsAfterCommand(szContent);
	if (!sName.empty()) {
		sName.erase(0, 1);
	}
	sName.erase(0, std::strlen(Safe(args.Arg(1))));
	const std::string sOldName = ClientName(iTarget);
	if (!ControllerValid(iTarget) || !__s2sdk_SetClientName) {
		return true;
	}
	const int iMessageType = GetMessageType();
	if (iMessageType == 2) {
		PrintChatAllPhrase("AdminRenameMessage", ActorName(iSlot).c_str(), sOldName.c_str(),
						   sName.c_str());
	} else if (iMessageType == 1) {
		PrintChatAllPhrase("RenameMessage", sOldName.c_str(), sName.c_str());
	}
	try {
		s2sdk::SetClientName(iTarget, Plg(sName));
	} catch (const std::exception& ex) {
		LogError(std::string("mm_rename failed: ") + ex.what());
		return true;
	} catch (...) {
		LogError("mm_rename failed with an unknown exception");
		return true;
	}
	SendAction(iSlot, "rename", std::to_string(iTarget) + " " + sName);
	return true;
}

bool ChangeTeamCommand(int32_t iSlot, const char* szContent, bool bSwitch, const char* szPermission) {
	if (iSlot < -1 || iSlot >= kSlots) {
		return true;
	}
	const bool bConsole = iSlot == -1;
	if (!bConsole && !HasReadyPermission(iSlot, szPermission)) {
		return true;
	}
	CommandArgs args;
	args.Tokenize(szContent);
	if (args.ArgC() < 3) {
		PrintUsage(iSlot, "UsageSwitchTeam", args.Arg(0));
		return true;
	}
	const int32_t iTarget = FindUser(args.Arg(1));
	if (iTarget < 0) {
		return true;
	}
	const std::string sTeam = args.Arg(2) != nullptr ? args.Arg(2) : "";
	if (!sTeam.empty() && (sTeam.length() != 1 || !std::isdigit(static_cast<unsigned char>(sTeam[0])))) {
		PrintUsage(iSlot, "UsageSwitchTeam", args.Arg(0));
		return true;
	}
	ChangePlayerTeam(iSlot, iTarget, std::atoi(sTeam.c_str()), bSwitch);
	return true;
}

bool HandleChangeTeamCommand(int32_t iSlot, const char* szContent) {
	return ChangeTeamCommand(iSlot, szContent, false, "@admin/changeteam");
}

bool HandleSwitchTeamCommand(int32_t iSlot, const char* szContent) {
	return ChangeTeamCommand(iSlot, szContent, true, "@admin/switchteam");
}

bool HandleNoclipCommand(int32_t iSlot, const char* szContent) {
	if (iSlot < -1 || iSlot >= kSlots) {
		return true;
	}
	const bool bConsole = iSlot == -1;
	if (!bConsole && !HasReadyPermission(iSlot, "@admin/noclip")) {
		return true;
	}
	CommandArgs args;
	args.Tokenize(szContent);
	if (args.ArgC() < 2) {
		PrintUsage(iSlot, "UsageDefault", args.Arg(0));
		return true;
	}
	const int32_t iTarget = FindUser(args.Arg(1));
	if (iTarget < 0) {
		return true;
	}
	NoclipPlayer(iSlot, iTarget);
	return true;
}

bool HandleWhoCommand(int32_t iSlot, const char* szContent) {
	if (iSlot < -1 || iSlot >= kSlots) {
		return true;
	}
	const bool bConsole = iSlot == -1;
	if (!bConsole && !HasReadyPermission(iSlot, "@admin/who")) {
		return true;
	}
	CommandArgs args;
	args.Tokenize(szContent);
	if (args.ArgC() < 2) {
		PrintUsage(iSlot, "UsageDefault", args.Arg(0));
		return true;
	}
	const int32_t iTarget = FindUser(args.Arg(1));
	if (iTarget < 0) {
		return true;
	}
	WhoPlayer(iSlot, iTarget, true);
	return true;
}

s2sdk::ResultType OnKillCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
								const plg::vector<plg::string>& args) {
	return RunConsoleCommand(g_commands[kCmdKill], iSlot, eContext, args);
}

s2sdk::ResultType OnKickCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
								const plg::vector<plg::string>& args) {
	return RunConsoleCommand(g_commands[kCmdKick], iSlot, eContext, args);
}

s2sdk::ResultType OnSlapCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
								const plg::vector<plg::string>& args) {
	return RunConsoleCommand(g_commands[kCmdSlap], iSlot, eContext, args);
}

s2sdk::ResultType OnRenameCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
								  const plg::vector<plg::string>& args) {
	return RunConsoleCommand(g_commands[kCmdRename], iSlot, eContext, args);
}

s2sdk::ResultType OnChangeTeamCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
									  const plg::vector<plg::string>& args) {
	return RunConsoleCommand(g_commands[kCmdChangeTeam], iSlot, eContext, args);
}

s2sdk::ResultType OnSwitchTeamCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
									  const plg::vector<plg::string>& args) {
	return RunConsoleCommand(g_commands[kCmdSwitchTeam], iSlot, eContext, args);
}

s2sdk::ResultType OnNoclipCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
								  const plg::vector<plg::string>& args) {
	return RunConsoleCommand(g_commands[kCmdNoclip], iSlot, eContext, args);
}

s2sdk::ResultType OnWhoCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
							   const plg::vector<plg::string>& args) {
	return RunConsoleCommand(g_commands[kCmdWho], iSlot, eContext, args);
}

void RegisterCommands() {
	// A7, paired bindings: AddConsoleCommand alone is not enough. Whether a name is ours (created)
	// or somebody else's (only a listener was attached) is known only after the call, so without
	// both RemoveCommand (for a created name) and RemoveCommandListener (for a pre-existing one)
	// OnPluginEnd could leave a callback pointing into the unloaded library. No registration at
	// all is the only safe answer when any of the three is missing.
	if (!__s2sdk_AddConsoleCommand || !__s2sdk_RemoveCommand ||
		!__s2sdk_RemoveCommandListener) {
		LogError("s2sdk AddConsoleCommand/RemoveCommand/RemoveCommandListener are not all "
				 "available - no mm_* command is registered (its callback could not be removed "
				 "on unload)");
		return;
	}
	constexpr s2sdk::ConVarFlag kFlags = static_cast<s2sdk::ConVarFlag>(
		static_cast<int64_t>(s2sdk::ConVarFlag::Release) |
		static_cast<int64_t>(s2sdk::ConVarFlag::ClientCanExecute));
	for (CommandDef& def : g_commands) {
		if (def.registered) {
			continue;
		}
		// false = the name already existed (engine or another plugin) and s2sdk only attached a
		// listener to it: OnPluginEnd must then detach the listener and never RemoveCommand a
		// command this plugin did not create (PITFALLS C11).
		def.created = s2sdk::AddConsoleCommand(def.szName, "", kFlags, def.callback,
											   s2sdk::HookMode::Pre);
		def.registered = true;
	}
}

void UnregisterCommands() {
	for (CommandDef& def : g_commands) {
		if (!def.registered) {
			continue;
		}
		if (def.created) {
			if (__s2sdk_RemoveCommand) {
				s2sdk::RemoveCommand(def.szName, def.callback);
			}
		} else if (__s2sdk_RemoveCommandListener) {
			// The name existed before this plugin: detach our listener, leave the command.
			s2sdk::RemoveCommandListener(def.szName, def.callback, s2sdk::HookMode::Pre);
		}
		def.registered = false;
		def.created = false;
	}
}

// ---------------------------------------------------------------------------------------------
// The admin core listener.
// ---------------------------------------------------------------------------------------------
void RegisterAdminMenuItems() {
	if (!g_adminBindingsAvailable || !g_menuBindingsAvailable || g_pAdminApi == nullptr) {
		return;
	}
	// The category itself is registered through the generated binding with a null display
	// callback: the consumer wrapper always installs its CategoryDisplayTrampoline (a pointer
	// into this .so), and the category is then a callback the core would hold after an unload.
	// Only the module's own "players" category is registered here; other plugins' categories and
	// items are never touched.
	if (__admin_system_RegisterCategory == nullptr) {
		LogError("the admin_system RegisterCategory binding is unavailable - the players "
				 "category is not registered");
		return;
	}
	g_adminMenuItemsRegistered = true;
	::admin_system::RegisterCategory(plg::string("players"), plg::string("Category_Players"),
									 nullptr);
	for (const ItemDef& item : kItems) {
		g_pAdminApi->RegisterItem(item.szIdentity, item.szPhrase, "players", item.szPermission,
								  nullptr, &OnAdminItemSelect);
	}
}

// The admin core has no unregister API for categories/items. Re-register every item this module
// registered with the same identity/name/category/flags and null callbacks, through the
// *generated binding*: the consumer wrapper cannot be used here because it always installs its
// own ItemDisplayTrampoline/ItemSelectTrampoline, i.e. pointers into the .so that is about to be
// unloaded. The core explicitly handles null callbacks (ItemLabel skips a null display and
// ShowAdminItemMenu refuses a null select), so the entries stay listed but do nothing until a
// reload registers the real ones. Only the identities this module registered are touched here -
// no other plugin's items or categories are re-registered, overwritten or removed. PITFALLS A7.
void RetireAdminMenuItems() {
	if (!g_adminMenuItemsRegistered) {
		return;
	}
	g_adminMenuItemsRegistered = false;
	if (__admin_system_RegisterItem == nullptr) {
		// Fail soft: without the binding the stale pointers cannot be cleared. Plugify unloads a
		// consumer before its dependency, so this only happens with an already-destroyed core.
		LogError("the admin_system RegisterItem binding is unavailable - the menu item callbacks "
				 "cannot be retired");
		return;
	}
	for (const ItemDef& item : kItems) {
		try {
			::admin_system::RegisterItem(plg::string(item.szIdentity), plg::string(item.szPhrase),
										 plg::string("players"), plg::string(item.szPermission),
										 nullptr, nullptr);
		} catch (const std::exception& ex) {
			LogError(std::string("could not retire the item '") + item.szIdentity + "': " + ex.what());
		} catch (...) {
			LogError(std::string("could not retire the item '") + item.szIdentity);
		}
	}
}

// The original registered the menu items when the admin core signalled that it is loaded. A
// module loaded after the core has to run the same block immediately: the event does not fire
// twice (plugins/admin_system/API.md).
void OnAdminCoreLoaded() {
	try {
		RegisterAdminMenuItems();
	} catch (const std::exception& ex) {
		LogError(std::string("RegisterAdminMenuItems failed: ") + ex.what());
	} catch (...) {
		LogError("RegisterAdminMenuItems failed with an unknown exception");
	}
}

// ---------------------------------------------------------------------------------------------
// Map listeners (PITFALLS B1/B2). They only touch the gate and this plugin's own bookkeeping.
// ---------------------------------------------------------------------------------------------
void OnMapStartListener() {
	g_mapReady = true;
}

void OnMapEndListener() {
	g_mapReady = false;
	g_mapEndedSinceStart = true;
	DropMenusForMapEnd();
}

// ---------------------------------------------------------------------------------------------
// Plugin glue
// ---------------------------------------------------------------------------------------------
// Every menu whose session is still open when the plugin unloads is torn down here. While a map
// is up DestroyMenu() closes the backend UI properly (the backend lives in another plugin that
// is still loaded); outside a map the session is ended with Disconnect first, which skips the
// backend and never touches the client list being torn down (PITFALLS B2/C10).
void CloseAllMenusForUnload() {
	const bool bMapReady = g_mapReady;
	std::vector<DestroyRequest> requests;
	for (int32_t i = 0; i < kSlots; ++i) {
		const s2sdk::MenuId id = g_openMenus[static_cast<std::size_t>(i)].id;
		if (id != 0) {
			requests.push_back(DestroyRequest{id, i});
		}
	}
	for (const DestroyRequest& request : g_destroyQueue) {
		requests.push_back(request);
	}
	g_destroyQueue.clear();
	for (const DestroyRequest& request : requests) {
		if (!DestroyMenuChecked(request.id, request.slot, bMapReady)) {
			// The last chance: report it loudly instead of pretending the unload was clean.
			ReportUnresolvedMenu(request.id, request.slot, true);
		}
	}
	for (OpenMenu& menu : g_openMenus) {
		menu = OpenMenu{};
	}
	g_unresolvedMenus.clear();
	for (Pending& pending : g_pending) {
		pending = Pending{};
	}
}

class AsPlayersPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override {
		try {
			g_pAdminApi = &admin_system::AdminApi::Get();
			// A reload (plg reload) starts from a clean sheet; OnPluginEnd already unregistered
			// everything, so the flags are simply re-armed here (PITFALLS A7).
			g_mapReady = false;
			g_mapEndedSinceStart = false;
			g_firstUpdateDone = false;
			g_adminMenuItemsRegistered = false;
			for (OpenMenu& menu : g_openMenus) {
				menu = OpenMenu{};
			}
			for (Pending& pending : g_pending) {
				pending = Pending{};
			}
			g_destroyQueue.clear();
			g_unresolvedMenus.clear();

			g_adminBindingsAvailable = AdminApiAvailable();
			g_menuBindingsAvailable = MenuApiAvailable();
			if (!g_adminBindingsAvailable) {
				LogError("the admin_system bindings are unavailable - permissions can only be "
						 "checked for nobody, so only the server console can use the commands");
			}
			if (!g_menuBindingsAvailable) {
				LogError("the s2sdk menu bindings are unavailable - the admin menu items are not "
						 "registered");
			}

			// PITFALLS B1 + A7: registering the map listeners touches no client, and nothing
			// could ever open the gate again without them. Only register when BOTH pairs exist -
			// the register halves and the matching unregister halves: a missing unregister would
			// leave a callback inside the unloaded library, so the gate simply stays shut.
			const bool bMapListenerBindings =
				__s2sdk_OnMapStart_Register != nullptr && __s2sdk_OnMapStart_Unregister != nullptr &&
				__s2sdk_OnMapEnd_Register != nullptr && __s2sdk_OnMapEnd_Unregister != nullptr;
			if (!g_mapListeners && bMapListenerBindings) {
				s2sdk::OnMapStart_Register(&OnMapStartListener);
				s2sdk::OnMapEnd_Register(&OnMapEndListener);
				g_mapListeners = true;
			} else if (!bMapListenerBindings) {
				LogError("the s2sdk map listener bindings are incomplete (register/unregister "
						 "pairs) - client calls stay disabled");
			}

			RegisterCommands();

			if (g_adminBindingsAvailable && g_pAdminApi != nullptr) {
				g_pAdminApi->OnCoreLoaded(&OnAdminCoreLoaded);
				if (g_pAdminApi->IsCoreLoaded()) {
					OnAdminCoreLoaded(); // late load: the event will not fire twice
				}
			}

			LogInfo("ready");
		} catch (const std::exception& ex) {
			LogError(std::string("OnPluginStart failed: ") + ex.what());
		} catch (...) {
			LogError("OnPluginStart failed with an unknown exception");
		}
		return {}; // never Failed (PITFALLS A1: that stops every Plugify plugin)
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds) override {
		try {
			// A plugin loaded into a running map never saw OnMapStart: its first update is the
			// "a map is up" signal (PITFALLS B1). An OnMapEnd seen since OnPluginStart means the
			// server is between maps, so the latch waits for the next OnMapStart instead.
			if (!g_firstUpdateDone) {
				g_firstUpdateDone = true;
				if (g_mapListeners && !g_mapEndedSinceStart) {
					g_mapReady = true;
				}
			}
			DrainDestroyQueue();
			// Menu selections are applied here, outside the s2sdk menu handler (PITFALLS C10):
			// no engine action and no admin callback runs under the menu manager's mutex, and
			// none of this runs under the s2sdk timer mutex (the plugin has no timers at all).
			for (int32_t i = 0; i < kSlots; ++i) {
				ApplyPending(i);
			}
		} catch (const std::exception& ex) {
			LogError(std::string("OnPluginUpdate failed: ") + ex.what());
		} catch (...) {
			LogError("OnPluginUpdate failed with an unknown exception");
		}
		return {};
	}

	plg::PluginResult OnPluginEnd() override {
		try {
			// Menus first: DestroyMenu reaches this plugin's handler, which must still be loaded.
			CloseAllMenusForUnload();
			// Then clear the admin core's item callbacks: they point into this .so (PITFALLS A7).
			RetireAdminMenuItems();

			g_mapReady = false;
			g_mapEndedSinceStart = true;
			g_firstUpdateDone = false;

			// A7, paired binding: only remove the listener this plugin actually registered, and
			// only while the unregister export exists. The consumer wrapper checks the plugin's
			// availability but not this individual slot, so the pair is guarded here.
			if (g_pAdminApi != nullptr && g_adminBindingsAvailable &&
				__admin_system_OnCoreLoaded_Unregister != nullptr) {
				g_pAdminApi->OnCoreLoadedUnregister();
			}

			UnregisterCommands();

			if (g_mapListeners) {
				if (__s2sdk_OnMapStart_Unregister) {
					s2sdk::OnMapStart_Unregister(&OnMapStartListener);
				}
				if (__s2sdk_OnMapEnd_Unregister) {
					s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
				}
				g_mapListeners = false;
			}

			// A stale callback (the engine still holds the pointer for a moment) can no longer
			// reach a client or an admin check.
			g_pAdminApi = nullptr;
			g_adminBindingsAvailable = false;
			g_menuBindingsAvailable = false;
			for (OpenMenu& menu : g_openMenus) {
				menu = OpenMenu{};
			}
			for (Pending& pending : g_pending) {
				pending = Pending{};
			}
			g_destroyQueue.clear();
		} catch (const std::exception& ex) {
			LogError(std::string("OnPluginEnd failed: ") + ex.what());
		} catch (...) {
			LogError("OnPluginEnd failed with an unknown exception");
		}
		return {};
	}
};

AsPlayersPlugin g_asPlayersPlugin;

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_asPlayersPlugin)
