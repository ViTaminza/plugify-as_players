// as_players tests — no engine: the plugin TU is compiled in (its handlers live in an anonymous
// namespace) and every __s2sdk_* / __admin_system_* binding slot is a recording fake.
//
// Covered:
//   * command ownership and contexts (PITFALLS C11): the eight commands are created with
//     ClientCanExecute|Release, a chat "!mm_kill" is left to s2sdk (Continue), a client console
//     caller needs the permission, the server console does not, and OnPluginEnd removes a name
//     this plugin created with RemoveCommand but only detaches a listener from a pre-existing
//     name;
//   * permissions: a denied client never reaches an action or SendAction, a revoked permission
//     stops an already-open menu selection, the admin-data readiness gate (API 1.1.0) delays
//     every client action and hides mm_who data until the core's SELECT for the live SteamID64
//     answered, and only slot -1 is the server-console bypass (-2/out-of-range fail closed);
//   * signed CEntityHandle semantics (PITFALLS C1): negative controller and pawn handles are
//     valid and reach commands and deferred menu actions end-to-end; only 0/-1 (and engine-
//     rejected handles) fail closed; the encode/decode identity round-trips the full int32 range;
//   * the player list: live controller+pawn slots only, dead players excluded from the
//     kill/slap/noclip lists but present for the others, bots listed with their "no SteamID64"
//     identity, SourceTV/unauthenticated slots absent, the noclip marker, pagination;
//   * slot reuse: a selection carries the target SteamID64 and never acts on the next occupant
//     of the slot (and a bot entry only acts while the slot is still a bot);
//   * the map gate (PITFALLS B1/B2): no client call before the first map, between OnMapEnd and
//     the next OnMapStart, or when the plugin was loaded into the map-change window; OnMapEnd
//     only drops bookkeeping and the queued menus are destroyed outside the teardown;
//   * the menu lifecycle (PITFALLS C10): MenuId 0 is burnt and re-created, the per-player
//     "!menu" type from the optional hudmenu API with the exact Pisex table for every choice and
//     backend combination (hud/button), exit-back/exit/
//     timeout/disconnect, deferred destroy of every menu, no engine action inside the menu
//     handler, and a menu whose display is refused is destroyed immediately;
//   * safe destruction (PITFALLS B2/A7): a CancelClientMenu(Disconnect) refused in a map-down
//     window is re-queried and the menu is switched to a builtin no-op-close type before
//     DestroyMenu (no backend close reaches the client layer); when even that is impossible the
//     request is parked and A7_UNRESOLVED is logged instead of blindly destroying or pretending;
//   * paired bindings / A7 registration: commands are only registered when AddConsoleCommand and
//     both removal paths exist, map listeners only with both register *and* both unregister
//     halves, the admin core listener only with its Register/Unregister pair - a partial pair
//     produces zero registrations (no dangling callback); a null admin_system, a null s2sdk menu
//     API and all bindings null at once still start, update and unload without a crash;
//   * unload/reload (PITFALLS A7): symmetric removal, open menus destroyed, the admin core's
//     item callbacks re-registered with null pointers through the low-level binding (all eight
//     identities), a missing binding fail-soft, stale callbacks inert, a second start works.
//
// Run by ./test.sh (build/tests/as_players_tests).

#include "../src/plugin.cpp"

#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

plg::string PlgTest(const std::string& text) {
	return plg::string(text.data(), text.size());
}

bool HasFlag(s2sdk::ConVarFlag flags, s2sdk::ConVarFlag flag) {
	return (static_cast<int64_t>(flags) & static_cast<int64_t>(flag)) != 0;
}

// --- the fake world -------------------------------------------------------------------------

struct FakeClient {
	bool connected = false;
	bool inGame = false;
	bool sourceTV = false;
	bool fake = false;
	bool alive = true;
	bool hasController = true;
	bool hasPawn = true;
	int32_t generation = 0; // s2sdk's entity serial: a reconnect gets a new one
	std::string name;
	uint64_t steamid = 0;
	s2sdk::MoveType moveType = s2sdk::MoveType::Walk;
	int32_t health = 100;
	plg::vec3 velocity{};
};

std::vector<FakeClient> g_clients(64);

int g_clientCalls = 0; // every fake that is a client query/output counts into this

const FakeClient& FakeSlot(int32_t slot) {
	static const FakeClient empty;
	return slot >= 0 && slot < static_cast<int32_t>(g_clients.size())
			   ? g_clients[static_cast<std::size_t>(slot)]
			   : empty;
}

void ResetClientCounters() {
	g_clientCalls = 0;
}

FakeClient& MakeHuman(int32_t slot, const std::string& name, uint64_t steamid, bool alive = true) {
	FakeClient& client = g_clients[static_cast<std::size_t>(slot)];
	client.connected = true;
	client.inGame = true;
	client.sourceTV = false;
	client.fake = false;
	client.alive = alive;
	client.hasController = true;
	client.hasPawn = alive;
	client.name = name;
	client.steamid = steamid;
	client.moveType = s2sdk::MoveType::Walk;
	client.health = 100;
	client.velocity = plg::vec3{0.0f, 0.0f, 0.0f};
	return client;
}

FakeClient& MakeBot(int32_t slot, const std::string& name, bool alive = true) {
	FakeClient& client = MakeHuman(slot, name, 0, alive);
	client.fake = true;
	return client;
}

void ClearClients() {
	g_clients.assign(64, FakeClient{});
}

// --- the fake map lifecycle ------------------------------------------------------------------

s2sdk::OnMapStartCallback g_onMapStart = nullptr;
s2sdk::OnMapEndCallback g_onMapEnd = nullptr;
int g_mapStartRegisters = 0;
int g_mapStartUnregisters = 0;
int g_mapEndRegisters = 0;
int g_mapEndUnregisters = 0;

void FakeMapStartRegister(s2sdk::OnMapStartCallback callback) {
	g_onMapStart = callback;
	++g_mapStartRegisters;
}

void FakeMapStartUnregister(s2sdk::OnMapStartCallback callback) {
	if (g_onMapStart == callback) {
		g_onMapStart = nullptr;
	}
	++g_mapStartUnregisters;
}

void FakeMapEndRegister(s2sdk::OnMapEndCallback callback) {
	g_onMapEnd = callback;
	++g_mapEndRegisters;
}

void FakeMapEndUnregister(s2sdk::OnMapEndCallback callback) {
	if (g_onMapEnd == callback) {
		g_onMapEnd = nullptr;
	}
	++g_mapEndUnregisters;
}

void FakeMapStart() {
	if (g_onMapStart != nullptr) {
		g_onMapStart();
	}
}

void FakeMapEnd() {
	if (g_onMapEnd != nullptr) {
		g_onMapEnd();
	}
}

// --- the fake client/entity layer ------------------------------------------------------------

bool FakeIsClientConnected(int32_t slot) {
	++g_clientCalls;
	return FakeSlot(slot).connected;
}

bool FakeIsClientInGame(int32_t slot) {
	++g_clientCalls;
	return FakeSlot(slot).inGame;
}

bool FakeIsClientSourceTV(int32_t slot) {
	++g_clientCalls;
	return FakeSlot(slot).sourceTV;
}

bool FakeIsFakeClient(int32_t slot) {
	++g_clientCalls;
	return FakeSlot(slot).fake;
}

bool FakeIsClientAlive(int32_t slot) {
	++g_clientCalls;
	return FakeSlot(slot).alive;
}

plg::string FakeGetClientName(int32_t slot) {
	++g_clientCalls;
	const FakeClient& client = FakeSlot(slot);
	return PlgTest(client.name.empty() ? "<unknown>" : client.name);
}

void FakeSetClientName(int32_t slot, const plg::string& name) {
	++g_clientCalls;
	if (slot >= 0 && slot < static_cast<int32_t>(g_clients.size())) {
		g_clients[static_cast<std::size_t>(slot)].name = ToStd(name);
	}
}

uint64_t FakeGetClientSteamID64(int32_t slot) {
	++g_clientCalls;
	return FakeSlot(slot).steamid;
}

std::map<int32_t, int32_t> g_slotHandleOverrides; // per-slot fake handle (0/-1/negative tests)
std::map<int32_t, int32_t> g_entityHandleOverrides; // per-index fake handle (pawn handle tests)
std::set<int32_t> g_rejectedHandles; // handles the fake engine says are invalid

int32_t FakePlayerSlotToEntHandle(int32_t slot) {
	++g_clientCalls;
	const FakeClient& client = FakeSlot(slot);
	if (!client.connected || !client.hasController) {
		return -1;
	}
	const auto override = g_slotHandleOverrides.find(slot);
	if (override != g_slotHandleOverrides.end()) {
		return override->second;
	}
	// index | serial << 15, the s2sdk CEntityHandle layout; the serial's bit 31 makes the handle
	// negative and that is still a valid handle (so the bit pattern is built unsigned-bitcast).
	const uint32_t value = static_cast<uint32_t>(slot + 1) |
						   (static_cast<uint32_t>(client.generation) << 15);
	return std::bit_cast<int32_t>(value);
}

bool FakeIsValidEntHandle(int32_t handle) {
	// The engine only rejects the sentinels and stale/unknown handles, never "negative because
	// the serial set bit 31".
	if (handle == 0 || handle == -1) {
		return false;
	}
	return g_rejectedHandles.count(handle) == 0;
}

void* FakeGetClientPawn(int32_t slot) {
	++g_clientCalls;
	const FakeClient& client = FakeSlot(slot);
	if (!client.connected || !client.hasController || !client.hasPawn) {
		return nullptr;
	}
	return reinterpret_cast<void*>(static_cast<uintptr_t>(0x10000 + slot));
}

int32_t FakeEntPointerToEntIndex(void* pointer) {
	if (pointer == nullptr) {
		return -1;
	}
	return static_cast<int32_t>(reinterpret_cast<uintptr_t>(pointer) - 0x10000) + 1;
}

int32_t FakeEntIndexToEntHandle(int32_t index) {
	const auto override = g_entityHandleOverrides.find(index);
	if (override != g_entityHandleOverrides.end()) {
		return override->second;
	}
	return index > 0 ? index : -1;
}

int32_t FakeGetClientHealth(int32_t slot) {
	++g_clientCalls;
	return FakeSlot(slot).health;
}

void FakeSetClientHealth(int32_t slot, int32_t health) {
	++g_clientCalls;
	if (slot >= 0 && slot < static_cast<int32_t>(g_clients.size())) {
		g_clients[static_cast<std::size_t>(slot)].health = health;
	}
}

s2sdk::MoveType FakeGetClientMoveType(int32_t slot) {
	++g_clientCalls;
	return FakeSlot(slot).moveType;
}

void FakeSetClientMoveType(int32_t slot, s2sdk::MoveType type) {
	++g_clientCalls;
	if (slot >= 0 && slot < static_cast<int32_t>(g_clients.size())) {
		g_clients[static_cast<std::size_t>(slot)].moveType = type;
	}
}

plg::vec3 FakeGetClientAbsVelocity(int32_t slot) {
	++g_clientCalls;
	return FakeSlot(slot).velocity;
}

void FakeSetClientAbsVelocity(int32_t slot, const plg::vec3& velocity) {
	++g_clientCalls;
	if (slot >= 0 && slot < static_cast<int32_t>(g_clients.size())) {
		g_clients[static_cast<std::size_t>(slot)].velocity = velocity;
	}
}

struct FakeSimpleCall {
	int32_t slot = 0;
	int32_t number = 0;
	bool flag = false;
};

std::vector<FakeSimpleCall> g_forceSuicides;
std::vector<FakeSimpleCall> g_kicks;
std::vector<FakeSimpleCall> g_teamChanges; // number = team, flag = switch
struct FakeStateChange {
	int32_t handle = 0;
	std::string className;
	std::string field;
};
std::vector<FakeStateChange> g_stateChanges;

void FakeForcePlayerSuicide(int32_t slot, bool explode, bool force) {
	++g_clientCalls;
	g_forceSuicides.push_back(FakeSimpleCall{slot, 0, explode || force});
	if (slot >= 0 && slot < static_cast<int32_t>(g_clients.size())) {
		g_clients[static_cast<std::size_t>(slot)].alive = false;
	}
}

void FakeKickClient(int32_t slot, s2sdk::NetworkDisconnectionReason reason,
					const plg::string& message) {
	++g_clientCalls;
	(void)reason;
	(void)message;
	g_kicks.push_back(FakeSimpleCall{slot, 0, false});
}

void FakeSwitchClientTeam(int32_t slot, s2sdk::CSTeam team) {
	++g_clientCalls;
	g_teamChanges.push_back(FakeSimpleCall{slot, static_cast<int32_t>(team), true});
}

void FakeChangeClientTeam(int32_t slot, s2sdk::CSTeam team) {
	++g_clientCalls;
	g_teamChanges.push_back(FakeSimpleCall{slot, static_cast<int32_t>(team), false});
}

void FakeNetworkStateChanged(int32_t handle, const plg::string& className,
							 const plg::string& memberName) {
	g_stateChanges.push_back(FakeStateChange{handle, ToStd(className), ToStd(memberName)});
}

// --- the fake output ------------------------------------------------------------------------

struct FakePrint {
	int32_t slot = 0;
	std::string text;
};

std::vector<FakePrint> g_chatPrints;
std::vector<std::string> g_chatAllPrints;
std::vector<FakePrint> g_consolePrints;
std::vector<std::string> g_serverPrints;

void FakePrintToChat(int32_t slot, const plg::string& text) {
	++g_clientCalls;
	g_chatPrints.push_back(FakePrint{slot, ToStd(text)});
}

void FakePrintToChatAll(const plg::string& text) {
	++g_clientCalls;
	g_chatAllPrints.push_back(ToStd(text));
}

void FakePrintToConsole(int32_t slot, const plg::string& text) {
	++g_clientCalls;
	g_consolePrints.push_back(FakePrint{slot, ToStd(text)});
}

void FakePrintToServer(const plg::string& text) {
	// Not a client call: it must stay reachable even while no map is up.
	g_serverPrints.push_back(ToStd(text));
}

void ResetPrints() {
	g_chatPrints.clear();
	g_chatAllPrints.clear();
	g_consolePrints.clear();
	g_serverPrints.clear();
}

bool HasServerPrint(const std::string& needle) {
	for (const std::string& line : g_serverPrints) {
		if (line.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

bool HasConsolePrint(const std::string& needle) {
	for (const FakePrint& print : g_consolePrints) {
		if (print.text.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

bool HasChatAllPrint(const std::string& needle) {
	for (const std::string& line : g_chatAllPrints) {
		if (line.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

bool HasChatPrint(const std::string& needle) {
	for (const FakePrint& print : g_chatPrints) {
		if (print.text.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

// --- the fake command manager ---------------------------------------------------------------

struct FakeCommand {
	std::string name;
	std::string description;
	s2sdk::ConVarFlag flags = s2sdk::ConVarFlag::None;
	s2sdk::ConCommandCallback callback = nullptr;
	s2sdk::HookMode mode = s2sdk::HookMode::Pre;
};

struct FakeListener {
	std::string name;
	s2sdk::ConCommandCallback callback = nullptr;
	s2sdk::HookMode mode = s2sdk::HookMode::Pre;
};

std::vector<FakeCommand> g_addedCommands;
std::set<std::string> g_preexistingCommands;
std::vector<std::pair<std::string, s2sdk::ConCommandCallback>> g_removedCommands;
std::vector<FakeListener> g_addedListeners;
std::vector<FakeListener> g_removedListeners;

bool FakeAddConsoleCommand(const plg::string& name, const plg::string& description,
						   s2sdk::ConVarFlag flags, s2sdk::ConCommandCallback callback,
						   s2sdk::HookMode mode) {
	const std::string key = ToStd(name);
	g_addedCommands.push_back(FakeCommand{key, ToStd(description), flags, callback, mode});
	// false = the name already existed (the engine or another plugin); s2sdk then only attaches
	// the listener to that command.
	return g_preexistingCommands.count(key) == 0;
}

bool FakeRemoveCommand(const plg::string& name, s2sdk::ConCommandCallback callback) {
	g_removedCommands.emplace_back(ToStd(name), callback);
	return true;
}

bool FakeAddCommandListener(const plg::string& name, s2sdk::ConCommandCallback callback,
							s2sdk::HookMode mode) {
	g_addedListeners.push_back(FakeListener{ToStd(name), callback, mode});
	return true;
}

bool FakeRemoveCommandListener(const plg::string& name, s2sdk::ConCommandCallback callback,
							   s2sdk::HookMode mode) {
	g_removedListeners.push_back(FakeListener{ToStd(name), callback, mode});
	return true;
}

const FakeCommand* FindAddedCommand(const std::string& name) {
	const FakeCommand* found = nullptr;
	for (const FakeCommand& command : g_addedCommands) {
		if (command.name == name) {
			found = &command; // the most recent registration wins
		}
	}
	return found;
}

bool RemovedCommand(const std::string& name) {
	for (const auto& [removedName, callback] : g_removedCommands) {
		(void)callback;
		if (removedName == name) {
			return true;
		}
	}
	return false;
}

bool RemovedListener(const std::string& name, s2sdk::ConCommandCallback callback,
					 s2sdk::HookMode mode) {
	for (const FakeListener& listener : g_removedListeners) {
		if (listener.name == name && listener.callback == callback && listener.mode == mode) {
			return true;
		}
	}
	return false;
}

plg::vector<plg::string> ToArgs(std::initializer_list<const char*> words) {
	plg::vector<plg::string> args;
	for (const char* word : words) {
		args.emplace_back(word);
	}
	return args;
}

s2sdk::ResultType CallCommand(s2sdk::ConCommandCallback callback, int32_t caller,
							  s2sdk::ConCommandContext context,
							  std::initializer_list<const char*> args) {
	const plg::vector<plg::string> argv = ToArgs(args);
	return callback(caller, context, argv);
}

s2sdk::ResultType DispatchCommand(const std::string& name, int32_t caller,
								  s2sdk::ConCommandContext context,
								  std::initializer_list<const char*> args) {
	const FakeCommand* command = FindAddedCommand(name);
	Check(command != nullptr, "command \"" + name + "\" is registered with s2sdk");
	if (command == nullptr) {
		return s2sdk::ResultType::Continue;
	}
	return CallCommand(command->callback, caller, context, args);
}

// --- the fake menu manager ------------------------------------------------------------------

struct FakeMenuItem {
	std::string info;
	std::string display;
	s2sdk::MenuItemStyle style = s2sdk::MenuItemStyle::Default;
};

struct FakeMenu {
	s2sdk::MenuId id = 0;
	std::string title;
	std::string type;
	s2sdk::MenuHandlerCallback handler = nullptr;
	std::vector<FakeMenuItem> items;
	int32_t pagination = 7;
	bool exitButton = true;
	bool backButton = false;
	bool closeOnSelect = true;
};

std::vector<FakeMenu> g_menus;
std::array<s2sdk::MenuId, 64> g_clientMenu{};
std::array<int32_t, 64> g_clientOffset{};
s2sdk::MenuId g_nextMenuId = 0;
int g_createMenuCalls = 0;
int g_destroyMenuCalls = 0;
std::vector<s2sdk::MenuId> g_destroyedMenuIds;
std::set<std::string> g_menuTypes{"chat", "console", "centerhtml", "button"};
std::vector<std::pair<int32_t, s2sdk::MenuCancelReason>> g_cancelledMenus;
std::vector<std::pair<int32_t, s2sdk::MenuCancelReason>> g_cancelAttempts;
bool g_cancelFails = false;
bool g_cancelLies = false;
int g_liveBackendCloses = 0; // backend close() that would reach the client/entity layer
std::vector<std::string> g_backendCloseTypes;
int g_setMenuTypeCalls = 0;
bool g_setMenuTypeFails = false;
int g_handlerCalls = 0;
bool g_menuZeroBurned = false;

FakeMenu* FindMenu(s2sdk::MenuId id) {
	for (FakeMenu& menu : g_menus) {
		if (menu.id == id) {
			return &menu;
		}
	}
	return nullptr;
}

void EndClientMenuFake(int32_t slot, s2sdk::MenuCancelReason reason) {
	const s2sdk::MenuId id = g_clientMenu[static_cast<std::size_t>(slot)];
	if (id == 0) {
		return;
	}
	g_clientMenu[static_cast<std::size_t>(slot)] = 0;
	g_clientOffset[static_cast<std::size_t>(slot)] = 0;
	const FakeMenu* menu = FindMenu(id);
	// s2sdk's CloseClientDisplay(notifyBackend = reason != Disconnect) calls the menu type's
	// close(); the builtin chat/console closers are no-ops (menu_types_builtin.cpp).
	if (reason != s2sdk::MenuCancelReason::Disconnect && menu != nullptr &&
		menu->type != "chat" && menu->type != "console") {
		++g_liveBackendCloses;
		g_backendCloseTypes.push_back(menu->type);
	}
	const s2sdk::MenuHandlerCallback handler = menu != nullptr ? menu->handler : nullptr;
	if (handler != nullptr) {
		++g_handlerCalls;
		handler(id, s2sdk::MenuAction::Cancel, slot, static_cast<int32_t>(reason));
		handler(id, s2sdk::MenuAction::End, slot, 0);
	}
}

bool FakeDestroyMenu(s2sdk::MenuId id) {
	for (int32_t slot = 0; slot < static_cast<int32_t>(g_clientMenu.size()); ++slot) {
		if (g_clientMenu[static_cast<std::size_t>(slot)] == id) {
			EndClientMenuFake(slot, s2sdk::MenuCancelReason::Destroyed);
		}
	}
	for (auto it = g_menus.begin(); it != g_menus.end(); ++it) {
		if (it->id == id) {
			g_menus.erase(it);
			++g_destroyMenuCalls;
			g_destroyedMenuIds.push_back(id);
			if (id == 0) {
				g_menuZeroBurned = true;
			}
			return true;
		}
	}
	return false;
}

s2sdk::MenuId FakeCreateMenu(const plg::string& title, s2sdk::MenuHandlerCallback handler,
							 const plg::string& type) {
	++g_createMenuCalls;
	FakeMenu menu;
	menu.id = g_nextMenuId++;
	menu.title = ToStd(title);
	menu.type = ToStd(type);
	menu.handler = handler;
	g_menus.push_back(std::move(menu));
	return g_menus.back().id;
}

int32_t FakeAddMenuItem(s2sdk::MenuId id, const plg::string& info, const plg::string& display,
						s2sdk::MenuItemStyle style) {
	FakeMenu* menu = FindMenu(id);
	if (menu == nullptr) {
		return -1;
	}
	menu->items.push_back(FakeMenuItem{ToStd(info), ToStd(display), style});
	return static_cast<int32_t>(menu->items.size()) - 1;
}

bool FakeSetMenuPagination(s2sdk::MenuId id, int32_t itemsPerPage) {
	FakeMenu* menu = FindMenu(id);
	if (menu == nullptr) {
		return false;
	}
	menu->pagination = itemsPerPage;
	return true;
}

bool FakeSetMenuExitButton(s2sdk::MenuId id, bool enabled) {
	FakeMenu* menu = FindMenu(id);
	if (menu == nullptr) {
		return false;
	}
	menu->exitButton = enabled;
	return true;
}

bool FakeSetMenuExitBackButton(s2sdk::MenuId id, bool enabled) {
	FakeMenu* menu = FindMenu(id);
	if (menu == nullptr) {
		return false;
	}
	menu->backButton = enabled;
	return true;
}

bool FakeSetMenuCloseOnSelect(s2sdk::MenuId id, bool enabled) {
	FakeMenu* menu = FindMenu(id);
	if (menu == nullptr) {
		return false;
	}
	menu->closeOnSelect = enabled;
	return true;
}

bool FakeDisplayMenuFail(s2sdk::MenuId id, int32_t slot, int32_t time) {
	(void)id;
	(void)slot;
	(void)time;
	return false;
}

bool FakeDisplayMenu(s2sdk::MenuId id, int32_t slot, int32_t time) {
	(void)time;
	FakeMenu* menu = FindMenu(id);
	if (menu == nullptr || slot < 0 || slot >= static_cast<int32_t>(g_clientMenu.size())) {
		return false;
	}
	if (g_clientMenu[static_cast<std::size_t>(slot)] != 0) {
		EndClientMenuFake(slot, s2sdk::MenuCancelReason::Interrupted);
	}
	g_clientMenu[static_cast<std::size_t>(slot)] = id;
	g_clientOffset[static_cast<std::size_t>(slot)] = 0;
	if (menu->handler != nullptr) {
		++g_handlerCalls;
		menu->handler(id, s2sdk::MenuAction::Start, slot, 0);
	}
	return true;
}

s2sdk::MenuId FakeGetClientMenu(int32_t slot) {
	if (slot < 0 || slot >= static_cast<int32_t>(g_clientMenu.size())) {
		return 0;
	}
	return g_clientMenu[static_cast<std::size_t>(slot)];
}

int32_t FakeGetClientMenuOffset(int32_t slot) {
	if (slot < 0 || slot >= static_cast<int32_t>(g_clientOffset.size())) {
		return 0;
	}
	return g_clientOffset[static_cast<std::size_t>(slot)];
}

bool FakeCancelClientMenu(int32_t slot, s2sdk::MenuCancelReason reason) {
	if (slot < 0 || slot >= static_cast<int32_t>(g_clientMenu.size()) ||
		g_clientMenu[static_cast<std::size_t>(slot)] == 0) {
		return false;
	}
	g_cancelAttempts.emplace_back(slot, reason);
	if (g_cancelFails) {
		return false; // the manager refused but the display is still active
	}
	if (g_cancelLies) {
		return true; // contract violation: true, but the display stays active
	}
	g_cancelledMenus.emplace_back(slot, reason);
	EndClientMenuFake(slot, reason);
	return true;
}

bool FakeIsValidMenu(s2sdk::MenuId id) {
	return FindMenu(id) != nullptr;
}

bool FakeIsMenuTypeRegistered(const plg::string& name) {
	return g_menuTypes.count(ToStd(name)) != 0;
}

bool FakeSetMenuType(s2sdk::MenuId id, const plg::string& typeName) {
	++g_setMenuTypeCalls;
	if (g_setMenuTypeFails) {
		return false;
	}
	FakeMenu* menu = FindMenu(id);
	if (menu == nullptr) {
		return false;
	}
	menu->type = ToStd(typeName);
	return true;
}

plg::string FakeGetMenuType(s2sdk::MenuId id) {
	const FakeMenu* menu = FindMenu(id);
	return menu != nullptr ? PlgTest(menu->type) : plg::string();
}

// --- the optional hudmenu plugin API --------------------------------------------------------

std::array<std::string, 64> g_clientMenuTypes;
int g_hudmenuCalls = 0;

plg::string FakeGetClientMenuType(int32_t slot) {
	++g_hudmenuCalls;
	if (slot < 0 || slot >= static_cast<int32_t>(g_clientMenuTypes.size())) {
		return {};
	}
	return PlgTest(g_clientMenuTypes[static_cast<std::size_t>(slot)]);
}

bool FakeSetClientMenuType(int32_t slot, const plg::string& type) {
	if (slot < 0 || slot >= static_cast<int32_t>(g_clientMenuTypes.size())) {
		return false;
	}
	g_clientMenuTypes[static_cast<std::size_t>(slot)] = ToStd(type);
	return true;
}

// --- the fake plg log ----------------------------------------------------------------------

std::vector<std::string> g_logs;

void FakeLog(std::string_view message, plg::Severity severity, const plg::source_location& location) {
	(void)severity;
	(void)location;
	g_logs.emplace_back(message);
}

bool HasLog(const std::string& needle) {
	for (const std::string& line : g_logs) {
		if (line.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

// --- the fake admin_system exports ----------------------------------------------------------

std::map<int32_t, std::set<std::string>> g_permissions;
std::map<int32_t, std::vector<std::string>> g_adminFlags;
std::map<std::string, std::string> g_flagNames;
std::map<std::string, std::string> g_translations;
std::set<int32_t> g_adminDataNotReady;
int32_t g_messageType = 0;
bool g_coreLoaded = true;

admin_system::CoreLoadedCallback g_coreLoadedCallback = nullptr;
int g_coreLoadedRegisters = 0;
int g_coreLoadedUnregisters = 0;

struct FakeCategory {
	std::string identity;
	std::string name;
	admin_system::CategoryDisplayCallback display = nullptr;
};

struct FakeItem {
	std::string identity;
	std::string name;
	std::string category;
	std::string flags;
	admin_system::ItemDisplayCallback display = nullptr;
	admin_system::ItemSelectCallback select = nullptr;
};

std::vector<FakeCategory> g_categories;
std::vector<FakeItem> g_items;
std::vector<int32_t> g_lastCategoryCalls;

struct FakeAction {
	int32_t slot = 0;
	std::string action;
	std::string param;
};

std::vector<FakeAction> g_actions;

float FakeGetPluginVersion() {
	return 1.1f;
}

plg::string FakeGetTranslation(const plg::string& key) {
	const auto it = g_translations.find(ToStd(key));
	return it != g_translations.end() ? PlgTest(it->second) : key;
}

int32_t FakeGetMessageType() {
	return g_messageType;
}

bool FakeIsCoreLoaded() {
	return g_coreLoaded;
}

bool FakeHasPermission(int32_t slot, const plg::string& permission) {
	const auto it = g_permissions.find(slot);
	return it != g_permissions.end() && it->second.count(ToStd(permission)) != 0;
}

plg::vector<plg::string> FakeGetAdminFlags(int32_t slot) {
	plg::vector<plg::string> values;
	const auto it = g_adminFlags.find(slot);
	if (it != g_adminFlags.end()) {
		for (const std::string& flag : it->second) {
			values.push_back(PlgTest(flag));
		}
	}
	return values;
}

plg::string FakeGetFlagName(const plg::string& flag) {
	const auto it = g_flagNames.find(ToStd(flag));
	return it != g_flagNames.end() ? PlgTest(it->second) : flag;
}

bool FakeIsClientAdminDataReady(int32_t slot, uint64_t steamid) {
	return slot >= 0 && slot < static_cast<int32_t>(g_clients.size()) &&
		   FakeSlot(slot).steamid == steamid && steamid != 0 &&
		   g_adminDataNotReady.count(slot) == 0;
}

void FakeSendAction(int32_t slot, const plg::string& action, const plg::string& param) {
	g_actions.push_back(FakeAction{slot, ToStd(action), ToStd(param)});
}

void FakeRegisterCategory(const plg::string& identity, const plg::string& name,
						  admin_system::CategoryDisplayCallback callback) {
	// The real core overwrites the entry with the same identity (AdminPlugin::RegisterCategory).
	for (FakeCategory& category : g_categories) {
		if (category.identity == ToStd(identity)) {
			category = FakeCategory{ToStd(identity), ToStd(name), callback};
			return;
		}
	}
	g_categories.push_back(FakeCategory{ToStd(identity), ToStd(name), callback});
}

void FakeRegisterItem(const plg::string& identity, const plg::string& name,
					  const plg::string& category, const plg::string& flags,
					  admin_system::ItemDisplayCallback display,
					  admin_system::ItemSelectCallback select) {
	// The core overwrites an item registered again (a reload path).
	for (FakeItem& item : g_items) {
		if (item.identity == ToStd(identity) && item.category == ToStd(category)) {
			item = FakeItem{ToStd(identity), ToStd(name), ToStd(category), ToStd(flags), display, select};
			return;
		}
	}
	g_items.push_back(
		FakeItem{ToStd(identity), ToStd(name), ToStd(category), ToStd(flags), display, select});
}

void FakeOnCoreLoadedRegister(admin_system::CoreLoadedCallback callback) {
	g_coreLoadedCallback = callback;
	++g_coreLoadedRegisters;
}

void FakeOnCoreLoadedUnregister(admin_system::CoreLoadedCallback callback) {
	if (g_coreLoadedCallback == callback) {
		g_coreLoadedCallback = nullptr;
	}
	++g_coreLoadedUnregisters;
}

void FakeShowAdminLastCategoryMenu(int32_t slot) {
	g_lastCategoryCalls.push_back(slot);
}

// --- binding installation -------------------------------------------------------------------

void InstallFakes() {
	__s2sdk_OnMapStart_Register = &FakeMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeMapEndUnregister;

	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsFakeClient = &FakeIsFakeClient;
	__s2sdk_IsClientAlive = &FakeIsClientAlive;
	__s2sdk_GetClientName = &FakeGetClientName;
	__s2sdk_SetClientName = &FakeSetClientName;
	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;
	__s2sdk_PlayerSlotToEntHandle = &FakePlayerSlotToEntHandle;
	__s2sdk_IsValidEntHandle = &FakeIsValidEntHandle;
	__s2sdk_GetClientPawn = &FakeGetClientPawn;
	__s2sdk_EntPointerToEntIndex = &FakeEntPointerToEntIndex;
	__s2sdk_EntIndexToEntHandle = &FakeEntIndexToEntHandle;
	__s2sdk_GetClientHealth = &FakeGetClientHealth;
	__s2sdk_SetClientHealth = &FakeSetClientHealth;
	__s2sdk_GetClientMoveType = &FakeGetClientMoveType;
	__s2sdk_SetClientMoveType = &FakeSetClientMoveType;
	__s2sdk_GetClientAbsVelocity = &FakeGetClientAbsVelocity;
	__s2sdk_SetClientAbsVelocity = &FakeSetClientAbsVelocity;
	__s2sdk_ForcePlayerSuicide = &FakeForcePlayerSuicide;
	__s2sdk_KickClient = &FakeKickClient;
	__s2sdk_SwitchClientTeam = &FakeSwitchClientTeam;
	__s2sdk_ChangeClientTeam = &FakeChangeClientTeam;
	__s2sdk_NetworkStateChanged = &FakeNetworkStateChanged;

	__s2sdk_PrintToChat = &FakePrintToChat;
	__s2sdk_PrintToChatAll = &FakePrintToChatAll;
	__s2sdk_PrintToConsole = &FakePrintToConsole;
	__s2sdk_PrintToServer = &FakePrintToServer;

	__s2sdk_AddConsoleCommand = &FakeAddConsoleCommand;
	__s2sdk_RemoveCommand = &FakeRemoveCommand;
	__s2sdk_AddCommandListener = &FakeAddCommandListener;
	__s2sdk_RemoveCommandListener = &FakeRemoveCommandListener;

	__s2sdk_CreateMenu = &FakeCreateMenu;
	__s2sdk_DestroyMenu = &FakeDestroyMenu;
	__s2sdk_AddMenuItem = &FakeAddMenuItem;
	__s2sdk_IsValidMenu = &FakeIsValidMenu;
	__s2sdk_SetMenuPagination = &FakeSetMenuPagination;
	__s2sdk_SetMenuExitButton = &FakeSetMenuExitButton;
	__s2sdk_SetMenuExitBackButton = &FakeSetMenuExitBackButton;
	__s2sdk_SetMenuCloseOnSelect = &FakeSetMenuCloseOnSelect;
	__s2sdk_DisplayMenu = &FakeDisplayMenu;
	__s2sdk_GetClientMenu = &FakeGetClientMenu;
	__s2sdk_GetClientMenuOffset = &FakeGetClientMenuOffset;
	__s2sdk_CancelClientMenu = &FakeCancelClientMenu;
	__s2sdk_IsMenuTypeRegistered = &FakeIsMenuTypeRegistered;
	__s2sdk_SetMenuType = &FakeSetMenuType;
	__s2sdk_GetMenuType = &FakeGetMenuType;

	__hudmenu_GetClientMenuType = &FakeGetClientMenuType;
	__hudmenu_SetClientMenuType = &FakeSetClientMenuType;

	plg::Log = &FakeLog;

	__admin_system_GetPluginVersion = &FakeGetPluginVersion;
	__admin_system_GetTranslation = &FakeGetTranslation;
	__admin_system_GetMessageType = &FakeGetMessageType;
	__admin_system_IsCoreLoaded = &FakeIsCoreLoaded;
	__admin_system_HasPermission = &FakeHasPermission;
	__admin_system_GetAdminFlags = &FakeGetAdminFlags;
	__admin_system_GetFlagName = &FakeGetFlagName;
	__admin_system_IsClientAdminDataReady = &FakeIsClientAdminDataReady;
	__admin_system_SendAction = &FakeSendAction;
	__admin_system_RegisterCategory = &FakeRegisterCategory;
	__admin_system_RegisterItem = &FakeRegisterItem;
	__admin_system_OnCoreLoaded_Register = &FakeOnCoreLoadedRegister;
	__admin_system_OnCoreLoaded_Unregister = &FakeOnCoreLoadedUnregister;
	__admin_system_ShowAdminLastCategoryMenu = &FakeShowAdminLastCategoryMenu;
}

// Forgets every fake registration and all recorded state. The plugin must have been stopped
// first (PrepareTest/FinishTest do that), otherwise it still believes its commands are open.
void ResetWorld() {
	ClearClients();
	g_permissions.clear();
	g_adminFlags.clear();
	g_flagNames.clear();
	g_adminDataNotReady.clear();
	g_slotHandleOverrides.clear();
	g_entityHandleOverrides.clear();
	g_rejectedHandles.clear();
	g_messageType = 0;
	g_coreLoaded = true;
	g_coreLoadedCallback = nullptr;
	g_clientMenuTypes = {};
	g_hudmenuCalls = 0;
	g_coreLoadedRegisters = 0;
	g_coreLoadedUnregisters = 0;
	g_categories.clear();
	g_items.clear();
	g_lastCategoryCalls.clear();
	g_actions.clear();

	g_forceSuicides.clear();
	g_kicks.clear();
	g_teamChanges.clear();
	g_stateChanges.clear();
	ResetClientCounters();
	ResetPrints();

	g_addedCommands.clear();
	g_preexistingCommands.clear();
	g_removedCommands.clear();
	g_addedListeners.clear();
	g_removedListeners.clear();

	g_menus.clear();
	g_clientMenu.fill(0);
	g_clientOffset.fill(0);
	g_nextMenuId = 0;
	g_createMenuCalls = 0;
	g_destroyMenuCalls = 0;
	g_destroyedMenuIds.clear();
	g_menuTypes = {"chat", "console", "centerhtml", "button"};
	g_cancelledMenus.clear();
	g_cancelAttempts.clear();
	g_cancelFails = false;
	g_cancelLies = false;
	g_liveBackendCloses = 0;
	g_backendCloseTypes.clear();
	g_setMenuTypeCalls = 0;
	g_setMenuTypeFails = false;
	g_handlerCalls = 0;
	g_logs.clear();
	g_menuZeroBurned = false;

	g_onMapStart = nullptr;
	g_onMapEnd = nullptr;
	g_mapStartRegisters = 0;
	g_mapStartUnregisters = 0;
	g_mapEndRegisters = 0;
	g_mapEndUnregisters = 0;

	g_translations = {
		{"Item_Who_Flags", "Player {LIGHTRED}%s{DEFAULT} has flags:"},
		{"Item_Who_NoFlags", "Player has no flags"},
		{"AdminKillMessage", "Admin {LIGHTRED}%s{DEFAULT} has killed player {LIGHTRED}%s{DEFAULT}"},
		{"KillMessage", "Admin has killed player {LIGHTRED}%s{DEFAULT}"},
		{"AdminKickMessage", "Admin {LIGHTRED}%s{DEFAULT} has kicked player {LIGHTRED}%s{DEFAULT}"},
		{"KickMessage", "Admin has kicked player {LIGHTRED}%s{DEFAULT}"},
		{"AdminRenameMessage",
		 "Admin {LIGHTRED}%s{DEFAULT} renamed player {LIGHTRED}%s{DEFAULT} to {LIGHTRED}%s{DEFAULT}"},
		{"RenameMessage", "Admin renamed player {LIGHTRED}%s{DEFAULT} to {LIGHTRED}%s{DEFAULT}"},
		{"AdminSlapMessage", "Admin {LIGHTRED}%s{DEFAULT} slapped player {LIGHTRED}%s{DEFAULT} ({LIGHTRED}%s{DEFAULT} damage)"},
		{"SlapMessage", "Admin slapped player {LIGHTRED}%s{DEFAULT} ({LIGHTRED}%s{DEFAULT} damage)"},
		{"AdminChangeTeamMessage",
		 "Admin {LIGHTRED}%s{DEFAULT} changed team of {LIGHTRED}%s{DEFAULT} to {LIGHTRED}%s{DEFAULT}"},
		{"ChangeTeamMessage", "Admin changed team of {LIGHTRED}%s{DEFAULT} to {LIGHTRED}%s{DEFAULT}"},
		{"AdminSwitchTeamMessage",
		 "Admin {LIGHTRED}%s{DEFAULT} switched {LIGHTRED}%s{DEFAULT} to {LIGHTRED}%s{DEFAULT}"},
		{"SwitchTeamMessage", "Admin switched {LIGHTRED}%s{DEFAULT} to {LIGHTRED}%s{DEFAULT}"},
		{"AdminNoclipMessageEnable", "Admin {LIGHTRED}%s{DEFAULT} enabled noclip for {LIGHTRED}%s{DEFAULT}"},
		{"AdminNoclipMessageDisable", "Admin {LIGHTRED}%s{DEFAULT} disabled noclip for {LIGHTRED}%s{DEFAULT}"},
		{"NoclipMessageEnable", "Admin enabled noclip for {LIGHTRED}%s{DEFAULT}"},
		{"NoclipMessageDisable", "Admin disabled noclip for {LIGHTRED}%s{DEFAULT}"},
		{"UsageDefault", "Usage: {LIGHTRED}%s{DEFAULT} <name/steamid/userid>"},
		{"UsageSlap", "Usage: {LIGHTRED}%s{DEFAULT} <name/steamid/userid> <damage>"},
		{"UsageRename", "Usage: {LIGHTRED}%s{DEFAULT} <name/steamid/userid> <name>"},
		{"UsageSwitchTeam", "Usage: {LIGHTRED}%s{DEFAULT} <name/steamid/userid> <0-3>"},
		{"none", "none"},
		{"spectators", "spectators"},
		{"terrorists", "terrorists"},
		{"counter-terrorists", "counter-terrorists"},
		{"SelectPlayer", "Select player"},
		{"SelectTeam", "Select team"},
		{"Item_Slap_Damage", "Select damage amount"},
		{"NoPlayers", "No available players"},
		{"Category_Players", "Players"},
		{"Item_Who", "Who"},
		{"Item_Noclip", "Noclip"},
		{"Item_SwitchTeam", "Switch team"},
		{"Item_ChangeTeam", "Change team"},
		{"Item_Rename", "Rename"},
		{"Item_Slap", "Slap"},
		{"Item_Kick", "Kick"},
		{"Item_Kill", "Kill"},
	};
	admin_system::AdminApi::Get().ReloadTranslations();
	InstallFakes();
}

// --- test lifecycle -------------------------------------------------------------------------

plg::PluginResult StartPlugin() {
	return g_asPlayersPlugin.OnPluginStart();
}

void Update() {
	g_asPlayersPlugin.OnPluginUpdate(std::chrono::milliseconds(16));
}

void StopPlugin() {
	g_asPlayersPlugin.OnPluginEnd();
}

// Stops whatever a previous test left behind and starts the next test from an empty fake world.
void PrepareTest() {
	StopPlugin();
	ResetWorld();
}

void FinishTest() {
	StopPlugin();
	ResetWorld();
}

// The plugin's own "a map is up" latch for the tests that do not model a full map boot.
void LatchMap() {
	Update();
}

void Grant(int32_t slot, const char* permission) {
	g_permissions[slot].insert(permission);
}

void Revoke(int32_t slot, const char* permission) {
	g_permissions[slot].erase(permission);
}

bool SelectAdminItem(int32_t slot, const char* identity) {
	for (const FakeItem& item : g_items) {
		if (item.identity == identity && item.select != nullptr) {
			item.select(slot, PlgTest(item.category), PlgTest(item.identity), PlgTest(item.name));
			return true;
		}
	}
	return false;
}

const FakeMenu* OpenMenuOf(int32_t slot) {
	return FindMenu(g_clientMenu[static_cast<std::size_t>(slot)]);
}

int32_t IndexOfItem(const FakeMenu& menu, const std::string& display) {
	for (std::size_t i = 0; i < menu.items.size(); ++i) {
		if (menu.items[i].display == display) {
			return static_cast<int32_t>(i);
		}
	}
	return -1;
}

// The s2sdk digit input: the item at |offset| + digit - 1. With SetMenuCloseOnSelect(false) the
// manager does not send End after Select, which is what the plugin asks for.
void SelectMenuItemFake(int32_t slot, int32_t absoluteIndex) {
	const FakeMenu* menu = OpenMenuOf(slot);
	if (menu == nullptr || menu->handler == nullptr) {
		return;
	}
	if (absoluteIndex < 0 || absoluteIndex >= static_cast<int32_t>(menu->items.size())) {
		return;
	}
	menu->handler(menu->id, s2sdk::MenuAction::Select, slot, absoluteIndex);
}

void SelectByDisplay(int32_t slot, const std::string& display) {
	const FakeMenu* menu = OpenMenuOf(slot);
	if (menu == nullptr) {
		Check(false, "a menu is open on slot " + std::to_string(slot));
		return;
	}
	const int32_t index = IndexOfItem(*menu, display);
	Check(index >= 0, "menu item \"" + display + "\" exists");
	if (index >= 0) {
		SelectMenuItemFake(slot, index);
	}
}

bool HasCategory(const char* identity) {
	for (const FakeCategory& category : g_categories) {
		if (category.identity == identity) {
			return true;
		}
	}
	return false;
}

const FakeItem* FindFakeItem(const char* identity) {
	for (const FakeItem& item : g_items) {
		if (item.identity == identity && item.category == "players") {
			return &item;
		}
	}
	return nullptr;
}

int ActionCount(const std::string& action) {
	int count = 0;
	for (const FakeAction& entry : g_actions) {
		count += entry.action == action ? 1 : 0;
	}
	return count;
}

const FakeAction* LastAction() {
	return g_actions.empty() ? nullptr : &g_actions.back();
}

// --- A. start, command ownership and contexts (PITFALLS C11) --------------------------------

void TestStartCommandsAndContexts() {
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok,
		  "OnPluginStart returns Ok (never Failed)");
	Check(g_mapStartRegisters == 1 && g_mapEndRegisters == 1, "both map listeners are registered");
	Check(!g_mapReady, "the map gate is shut on plugin start");
	Check(g_coreLoadedRegisters == 1, "the admin core listener is registered once");
	Check(HasCategory("players"), "the players category is registered");
	Check(g_items.size() == 8, "the eight admin items are registered");
	Check(g_addedCommands.size() == 8, "the eight console commands are created");

	for (const CommandDef& def : g_commands) {
		const FakeCommand* command = FindAddedCommand(def.szName);
		Check(command != nullptr, std::string(def.szName) + " is registered");
		if (command == nullptr) {
			continue;
		}
		Check(HasFlag(command->flags, s2sdk::ConVarFlag::ClientCanExecute),
			  std::string(def.szName) + " is client-executable (the permission check needs the caller)");
		Check(HasFlag(command->flags, s2sdk::ConVarFlag::Release),
			  std::string(def.szName) + " is a Release command");
		Check(command->mode == s2sdk::HookMode::Pre, std::string(def.szName) + " hooks Pre");
		Check(command->callback == def.callback, std::string(def.szName) + " keeps its callback");
	}

	// The gate has to be open for any client work; it was checked shut above.
	LatchMap();

	// --- a chat "!mm_kill" is not this console command --------------------------------
	{
		ResetPrints();
		const auto result = DispatchCommand("mm_kill", 10, s2sdk::ConCommandContext::Chat, {"mm_kill", "Admin"});
		Check(result == s2sdk::ResultType::Continue,
			  "a chat spelling of a console command is left to s2sdk (Continue)");
		Check(g_forceSuicides.empty(), "the chat spelling performs no action");
		Check(g_chatAllPrints.empty(), "the chat spelling prints nothing");
	}

	// --- a client console caller needs the permission ---------------------------------
	{
		MakeHuman(11, "Target", 76561198000000011ULL);
		const auto denied = DispatchCommand("mm_kill", 10, s2sdk::ConCommandContext::Console,
											{"mm_kill", "Target"});
		Check(denied == s2sdk::ResultType::Handled, "the command is owned even when denied");
		Check(g_forceSuicides.empty(), "an unpermitted client console caller performs no action");
		Check(ActionCount("kill") == 0, "an unpermitted call sends no admin action");

		Grant(10, "@admin/kill");
		const auto allowed = DispatchCommand("mm_kill", 10, s2sdk::ConCommandContext::Console,
											 {"mm_kill", "Target"});
		Check(allowed == s2sdk::ResultType::Handled, "the permitted client console call is handled");
		Check(g_forceSuicides.size() == 1 && g_forceSuicides[0].slot == 11,
			  "the permitted call kills the resolved target");
		Check(ActionCount("kill") == 1 && LastAction()->param == "11",
			  "the permitted call sends the original kill action");
	}

	// --- the server console is allowed without a permission ---------------------------
	{
		MakeHuman(12, "Second", 76561198000000012ULL);
		const auto result = DispatchCommand("mm_kick", -1, s2sdk::ConCommandContext::Console,
											{"mm_kick", "Second"});
		Check(result == s2sdk::ResultType::Handled, "the server console command is handled");
		Check(g_kicks.size() == 1 && g_kicks[0].slot == 12,
			  "the server console needs no permission (the original's caller < 0 path)");
		Check(ActionCount("kick") == 1, "the server console kick sends its action");
	}

	// --- usage messages ----------------------------------------------------------------
	{
		ResetPrints();
		DispatchCommand("mm_rename", -1, s2sdk::ConCommandContext::Console, {"mm_rename"});
		Check(HasServerPrint("Usage: mm_rename <name/steamid/userid> <name>"),
			  "the usage phrase is formatted with the command name");
		Check(!HasServerPrint("{LIGHTRED}"), "the server console output has no colour tags");
	}

	// --- the rename command keeps the original's raw-name quirk -----------------------
	{
		MakeHuman(13, "OldName", 76561198000000013ULL);
		ResetPrints();
		DispatchCommand("mm_rename", -1, s2sdk::ConCommandContext::Console,
						{"mm_rename", "13", "New", "Name"});
		Check(ToStd(FakeGetClientName(13)) == "New Name",
			  "mm_rename keeps the rest of the raw argument string (spaces preserved)");
		Check(LastAction() != nullptr && LastAction()->action == "rename" &&
				  LastAction()->param == "13 New Name",
			  "the rename action carries the new name");
	}

	// --- ownership: a pre-existing name is detached, never removed --------------------
	{
		FinishTest();
		g_preexistingCommands = {"mm_kill"};
		Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start with a pre-existing command name");
		StopPlugin();
		const FakeCommand* command = FindAddedCommand("mm_kill");
		Check(command != nullptr, "the pre-existing command was hooked");
		Check(!RemovedCommand("mm_kill"), "a command this plugin did not create is never RemoveCommand'd");
		Check(RemovedListener("mm_kill", command != nullptr ? command->callback : nullptr,
							  s2sdk::HookMode::Pre),
			  "the listener of a pre-existing command is detached");
		Check(RemovedCommand("mm_kick"), "a command this plugin created is removed");
		Check(g_mapStartUnregisters == 1 && g_mapEndUnregisters == 1,
			  "the map listeners are unregistered");
		Check(g_coreLoadedUnregisters == 1, "the admin core listener is unregistered");
	}

	FinishTest();
}

// --- B. permissions -------------------------------------------------------------------------

void TestPermissions() {
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	MakeHuman(11, "Target", 76561198000000011ULL);
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	LatchMap();

	// Every command denies the unpermitted client console caller.
	{
		ResetPrints();
		const std::array<std::pair<const char*, const char*>, 8> cases{{
			{"mm_kill", "Target"},
			{"mm_kick", "Target"},
			{"mm_slap", "Target 10"},
			{"mm_rename", "Target X"},
			{"mm_changeteam", "Target 2"},
			{"mm_switchteam", "Target 2"},
			{"mm_noclip", "Target"},
			{"mm_who", "Target"},
		}};
		for (const auto& [name, arguments] : cases) {
			CommandArgs split;
			split.Tokenize(arguments);
			plg::vector<plg::string> args;
			args.emplace_back(name);
			for (int i = 0; i < split.ArgC(); ++i) {
				args.emplace_back(split.Arg(i));
			}
			const FakeCommand* command = FindAddedCommand(name);
			Check(command != nullptr, std::string(name) + " is registered");
			if (command != nullptr) {
				Check(command->callback(10, s2sdk::ConCommandContext::Console, args) ==
						  s2sdk::ResultType::Handled,
					  std::string(name) + " is handled");
			}
		}
		Check(g_forceSuicides.empty() && g_kicks.empty() && g_teamChanges.empty(),
			  "not one denied command reaches an action");
		Check(g_actions.empty(), "not one denied command sends an admin action");
		Check(g_chatAllPrints.empty(), "not one denied command prints to chat");
		Check(!HasChatPrint("flags"), "not one denied command shows admin data");
	}

	// The same calls pass once the permission is granted (kill and kick as representatives).
	{
		Grant(10, "@admin/kill");
		Grant(10, "@admin/kick");
		DispatchCommand("mm_kill", 10, s2sdk::ConCommandContext::Console, {"mm_kill", "Target"});
		Check(g_forceSuicides.size() == 1, "the permitted kill reaches the target");
		DispatchCommand("mm_kick", 10, s2sdk::ConCommandContext::Console, {"mm_kick", "Target"});
		Check(g_kicks.size() == 1, "the permitted kick reaches the target");
	}

	// mm_who hides the data while the core's admin SELECT has not answered yet.
	{
		MakeHuman(10, "Admin", 76561198000000010ULL); // re-alive after the kill?
		Grant(10, "@admin/who");
		g_adminFlags[10] = {"b", "c"};
		g_flagNames["b"] = "Basic admin";
		g_flagNames["c"] = "CheckCheats";
		g_adminDataNotReady = {10};
		ResetPrints();
		DispatchCommand("mm_who", -1, s2sdk::ConCommandContext::Console, {"mm_who", "Admin"});
		Check(!HasServerPrint("Basic admin") && !HasServerPrint("has flags"),
			  "mm_who shows no flags while the admin data is not loaded");
		Check(ActionCount("who") == 0, "mm_who sends no action without data");

		g_adminDataNotReady.clear();
	g_slotHandleOverrides.clear();
	g_entityHandleOverrides.clear();
	g_rejectedHandles.clear();
		ResetPrints();
		DispatchCommand("mm_who", -1, s2sdk::ConCommandContext::Console, {"mm_who", "Admin"});
		Check(HasServerPrint("Basic admin") && HasServerPrint("CheckCheats"),
			  "mm_who shows the flags once the admin data is ready");
		Check(ActionCount("who") == 1, "mm_who sends its action once it shows data");
	}

	// Only slot -1 is the server console: -2 and out-of-range callers fail closed.
	{
		MakeHuman(11, "Target", 76561198000000011ULL);
		g_forceSuicides.clear();
		ResetPrints();
		for (const int32_t caller : {-2, -100, 64, 999}) {
			DispatchCommand("mm_kill", caller, s2sdk::ConCommandContext::Console,
							{"mm_kill", "Target"});
		}
		Check(g_forceSuicides.empty(), "an invalid caller never bypasses the permission check");
		Check(g_chatAllPrints.empty() && g_serverPrints.empty() && g_consolePrints.empty(),
			  "an invalid caller gets no output at all");
	}

	// The canonical, current-SteamID readiness gate: a granted permission is not used while the
	// admin core's asynchronous SELECT has not answered for this player yet.
	{
		MakeHuman(11, "ReadyTarget", 76561198000000012ULL);
		Grant(10, "@admin/kill");
		g_adminDataNotReady = {10};
		g_forceSuicides.clear();
		DispatchCommand("mm_kill", 10, s2sdk::ConCommandContext::Console,
						{"mm_kill", "ReadyTarget"});
		Check(g_forceSuicides.empty(),
			  "a granted permission is denied while the admin data is not ready");
		g_adminDataNotReady.clear();
	g_slotHandleOverrides.clear();
	g_entityHandleOverrides.clear();
	g_rejectedHandles.clear();
		DispatchCommand("mm_kill", 10, s2sdk::ConCommandContext::Console,
						{"mm_kill", "ReadyTarget"});
		Check(g_forceSuicides.size() == 1, "the same call works once the admin data is ready");

		g_forceSuicides.clear();
		g_adminDataNotReady = {10};
		Check(SelectAdminItem(10, "kill"), "kill item with the admin data pending");
		Update();
		Check(OpenMenuOf(10) == nullptr,
			  "no private menu is opened while the admin data is not ready");
		Check(g_forceSuicides.empty(), "no action runs while the admin data is not ready");
		g_adminDataNotReady.clear();
	g_slotHandleOverrides.clear();
	g_entityHandleOverrides.clear();
	g_rejectedHandles.clear();
	}

	// A permission revoked while the player list is open stops the pending selection.
	{
		MakeHuman(11, "Target", 76561198000000011ULL); // alive again for the menu list
		Grant(10, "@admin/kill");
		Check(SelectAdminItem(10, "kill"), "the kill item is registered");
		Update(); // opens the player list
		const FakeMenu* menu = OpenMenuOf(10);
		Check(menu != nullptr, "the player list opened");
		if (menu != nullptr) {
			SelectByDisplay(10, "Target");
		}
		Revoke(10, "@admin/kill");
		g_forceSuicides.clear();
		g_actions.clear();
		Update();
		Check(g_forceSuicides.empty(), "a revoked permission stops the deferred selection");
		Check(ActionCount("kill") == 0, "a revoked permission sends no admin action");
	}

	FinishTest();
}

// --- B2. signed CEntityHandle semantics (PITFALLS C1) ---------------------------------------

int32_t SignedHandle(uint32_t value) {
	return std::bit_cast<int32_t>(value);
}

void TestSignedHandles() {
	// Encode/decode: a negative signed handle round-trips; only int32 range and 0/-1 are refused.
	{
		const int32_t negative = SignedHandle(0x80000005u);
		const uint64_t steamid = 76561198000000001ULL;
		const std::string encoded = EncodeTarget(5, steamid, negative);
		Check(encoded == "5:" + std::to_string(steamid) + ":" + std::to_string(negative),
			  "a negative controller handle is encoded as a signed decimal");
		int32_t slot = -1;
		uint64_t decodedSteamId = 0;
		int32_t decodedHandle = 0;
		Check(DecodeTarget(encoded, slot, decodedSteamId, decodedHandle),
			  "the negative-handle identity decodes");
		Check(slot == 5 && decodedSteamId == steamid && decodedHandle == negative,
			  "the decoded identity equals the encoded one");
		Check(DecodeTarget("5:" + std::to_string(steamid) + ":" +
							   std::to_string(std::numeric_limits<int32_t>::min()),
						   slot, decodedSteamId, decodedHandle),
			  "INT32_MIN is inside the accepted int32 range");
		Check(!DecodeTarget("5:" + std::to_string(steamid) + ":0", slot, decodedSteamId,
							decodedHandle),
			  "handle 0 (sentinel) is refused");
		Check(!DecodeTarget("5:" + std::to_string(steamid) + ":-1", slot, decodedSteamId,
							decodedHandle),
			  "handle -1 (sentinel) is refused");
		Check(!DecodeTarget("5:" + std::to_string(steamid) + ":-2147483649", slot,
							decodedSteamId, decodedHandle),
			  "a value below INT32_MIN is refused");
		Check(!DecodeTarget("5:" + std::to_string(steamid) + ":2147483648", slot,
							decodedSteamId, decodedHandle),
			  "a value above INT32_MAX is refused");
	}

	// The controller handle itself: a negative handle is valid, only 0/-1/engine-reject are not.
	{
		PrepareTest();
		MakeHuman(10, "Admin", 76561198000000010ULL);
		g_clients[10].generation = 0x10000; // serial bit 16 -> handle bit 31 -> negative
		Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
		LatchMap();
		const int32_t negative = ControllerHandle(10);
		Check(negative != -1 && negative < 0, "a real negative controller handle is accepted");
		Check(ControllerValid(10), "the negative-handle controller is valid");

		g_slotHandleOverrides[10] = 0;
		Check(ControllerHandle(10) == -1 && !ControllerValid(10),
			  "controller handle 0 is a sentinel, not a handle");
		g_slotHandleOverrides[10] = -1;
		Check(ControllerHandle(10) == -1 && !ControllerValid(10),
			  "controller handle -1 is a sentinel, not a handle");

		const int32_t engineRejected = SignedHandle(0x8000000Au);
		g_slotHandleOverrides[10] = engineRejected;
		g_rejectedHandles.insert(engineRejected);
		Check(ControllerHandle(10) == -1 && !ControllerValid(10),
			  "a non-sentinel handle the engine rejects is invalid");

		g_rejectedHandles.clear();
		Check(ControllerHandle(10) == engineRejected && ControllerValid(10),
			  "the same negative handle is valid once the engine accepts it");
		FinishTest();
	}

	// End-to-end: a negative controller handle reaches the slap/noclip actions.
	{
		PrepareTest();
		MakeHuman(10, "Admin", 76561198000000010ULL);
		MakeHuman(11, "Target", 76561198000000011ULL);
		g_clients[10].generation = 0x10000; // negative
		g_clients[11].generation = 0x10001; // negative
		Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
		LatchMap();
		Grant(10, "@admin/slap");
		Grant(10, "@admin/noclip");

		Check(ControllerValid(10) && ControllerHandle(10) < 0,
			  "the actor has a negative controller handle");
		Check(ControllerValid(11) && ControllerHandle(11) < 0,
			  "the target has a negative controller handle");

		g_clients[11].health = 100;
		DispatchCommand("mm_slap", -1, s2sdk::ConCommandContext::Console, {"mm_slap", "11", "5"});
		Check(g_clients[11].health == 95, "slap works with negative controller handles");
		Check(ActionCount("slap") == 1, "the slap action was sent");

		// A negative pawn handle is a valid CEntityHandle too: the state change must use it.
		const int32_t pawnIndex = 12; // slot 11 -> pawn entity index 12 in the fake
		const int32_t negativePawnHandle = SignedHandle(0x8000000Cu);
		g_entityHandleOverrides[pawnIndex] = negativePawnHandle;
		g_stateChanges.clear();
		g_clients[11].health = 100;
		DispatchCommand("mm_slap", -1, s2sdk::ConCommandContext::Console, {"mm_slap", "11", "10"});
		Check(!g_stateChanges.empty() &&
				  g_stateChanges.back().handle == negativePawnHandle,
			  "the negative pawn handle reaches NetworkStateChanged");
		g_entityHandleOverrides.erase(pawnIndex);

		g_clients[11].moveType = s2sdk::MoveType::Walk;
		DispatchCommand("mm_noclip", -1, s2sdk::ConCommandContext::Console, {"mm_noclip", "11"});
		Check(g_clients[11].moveType == s2sdk::MoveType::Noclip,
			  "noclip works with negative controller handles");
		FinishTest();
	}

	// End-to-end menu: a negative actor AND target handle open the player list and get the kill.
	{
		PrepareTest();
		MakeHuman(10, "Admin", 76561198000000010ULL);
		MakeHuman(11, "Target", 76561198000000011ULL);
		g_clients[10].generation = 0x10000;
		g_clients[11].generation = 0x10001;
		Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
		LatchMap();
		Grant(10, "@admin/kill");
		Check(SelectAdminItem(10, "kill"), "kill item");
		Update();
		const FakeMenu* menu = OpenMenuOf(10);
		Check(menu != nullptr, "the player list opens for negative handles");
		Check(menu != nullptr && IndexOfItem(*menu, "Target") >= 0,
			  "the negative-handle target is listed");
		SelectByDisplay(10, "Target");
		Update();
		Check(g_forceSuicides.size() == 1 && g_forceSuicides[0].slot == 11,
			  "the negative-handle target gets the action end-to-end");
		Check(ActionCount("kill") == 1, "the kill action was sent for the negative handle");
		Check(OpenMenuOf(10) != nullptr, "the list is rebuilt after the action");
		FinishTest();
	}

	// Deferred identity comparison: a changed negative target/actor handle is not the same player.
	{
		PrepareTest();
		MakeHuman(10, "Admin", 76561198000000010ULL);
		MakeHuman(11, "Target", 76561198000000011ULL);
		g_clients[10].generation = 0x10000;
		g_clients[11].generation = 0x10001;
		Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
		LatchMap();
		Grant(10, "@admin/kill");
		Check(SelectAdminItem(10, "kill"), "kill item");
		Update();
		SelectByDisplay(10, "Target");
		g_clients[11].generation = 0x10002; // same SteamID64, new negative handle
		g_forceSuicides.clear();
		Update();
		Check(g_forceSuicides.empty(),
			  "a new negative target handle is a different connection - no action");
		Check(OpenMenuOf(10) != nullptr, "the list is rebuilt after the target reconnect");

		Check(SelectAdminItem(10, "kill"), "kill item again");
		Update();
		SelectByDisplay(10, "Target");
		g_clients[10].generation = 0x10003; // the admin's own connection changed
		g_forceSuicides.clear();
		Update();
		Check(g_forceSuicides.empty(),
			  "a new negative actor handle drops the deferred selection");
		Check(g_openMenus[10].id == 0, "the stale menu of the previous connection is closed");
		FinishTest();
	}

	// 0/-1 controller handles (and engine-rejected ones) fail closed end-to-end.
	{
		PrepareTest();
		MakeHuman(10, "Admin", 76561198000000010ULL);
		MakeHuman(11, "Target", 76561198000000011ULL);
		Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
		LatchMap();
		Grant(10, "@admin/kill");
		for (const int32_t sentinel : {0, -1}) {
			g_slotHandleOverrides[11] = sentinel;
			g_forceSuicides.clear();
			g_actions.clear();
			DispatchCommand("mm_kill", -1, s2sdk::ConCommandContext::Console,
							{"mm_kill", "Target"});
			Check(g_forceSuicides.empty(),
				  "a controller handle " + std::to_string(sentinel) +
					  " is a sentinel: no action");
			Check(ActionCount("kill") == 0, "no kill action for a sentinel handle");
		}
		const int32_t engineRejected = SignedHandle(0x8000000Bu);
		g_slotHandleOverrides[11] = engineRejected;
		g_rejectedHandles.insert(engineRejected);
		g_forceSuicides.clear();
		DispatchCommand("mm_kill", -1, s2sdk::ConCommandContext::Console, {"mm_kill", "Target"});
		Check(g_forceSuicides.empty(), "an engine-rejected handle means no action");
		FinishTest();
	}
}

// --- C. the player list ---------------------------------------------------------------------

void TestPlayerList() {
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	MakeHuman(0, "Alice", 76561198000000001ULL, true);
	MakeHuman(1, "DeadBob", 76561198000000002ULL, false);
	g_clients[1].hasPawn = true; // a dead player that still carries a pawn entity
	MakeBot(2, "Bot");
	MakeHuman(3, "SourceTV", 0);
	g_clients[3].sourceTV = true;
	MakeHuman(4, "Unauth", 0); // connected and in game, but no SteamID64 yet
	MakeHuman(5, "NoController", 76561198000000005ULL);
	g_clients[5].hasController = false;
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	LatchMap();
	Grant(10, "@admin/kill");
	Grant(10, "@admin/kick");
	Grant(10, "@admin/noclip");
	Grant(10, "@admin/slap");
	Grant(10, "@admin/who");

	// kill: alive only
	{
		Check(SelectAdminItem(10, "kill"), "kill item registered");
		Update();
		const FakeMenu* menu = OpenMenuOf(10);
		Check(menu != nullptr, "the kill player list opened");
		if (menu != nullptr) {
			Check(IndexOfItem(*menu, "Alice") >= 0, "a live human is listed for kill");
			Check(IndexOfItem(*menu, "Bot") >= 0, "a live bot is listed for kill");
			Check(IndexOfItem(*menu, "DeadBob") < 0, "a dead player is not listed for kill");
			Check(IndexOfItem(*menu, "SourceTV") < 0, "the SourceTV proxy is not listed");
			Check(IndexOfItem(*menu, "Unauth") < 0, "a slot without a SteamID64 is not listed");
			Check(IndexOfItem(*menu, "NoController") < 0, "a slot without a controller is not listed");
			Check(menu->pagination == 5, "the Pisex pagination (5 items per page) is set");
			Check(menu->exitButton && menu->backButton, "exit and back buttons are enabled");
			Check(!menu->closeOnSelect, "the menu stays open after a selection");
			Check(menu->type == "button",
				  "the Pisex HUD_LAYOUT fallback without the hud backend is button");
		}
		if (menu != nullptr) {
			SelectByDisplay(10, "Alice");
		}
		Update();
		Check(g_forceSuicides.size() == 1 && g_forceSuicides[0].slot == 0,
			  "the player list selected the right slot");
	}

	// kick: a dead player with a pawn is listed (the original only required a pawn)
	{
		Check(SelectAdminItem(10, "kick"), "kick item registered");
		Update();
		const FakeMenu* menu = OpenMenuOf(10);
		Check(menu != nullptr, "the kick player list opened");
		if (menu != nullptr) {
			Check(IndexOfItem(*menu, "DeadBob") >= 0, "the kick list includes the pawn-bearing dead player");
			Check(IndexOfItem(*menu, "Unauth") < 0, "kick does not list identity-less slots");
		}
	}

	// noclip: the [ON]/[OFF] marker comes from the pawn's move type
	{
		MakeHuman(0, "Alice", 76561198000000001ULL, true); // alive again after the kill test
		g_clients[0].moveType = s2sdk::MoveType::Noclip;
		Check(SelectAdminItem(10, "noclip"), "noclip item registered");
		Update();
		const FakeMenu* menu = OpenMenuOf(10);
		Check(menu != nullptr, "the noclip player list opened");
		if (menu != nullptr) {
			Check(IndexOfItem(*menu, "Alice [ON]") >= 0, "a noclip player is marked [ON]");
			Check(IndexOfItem(*menu, "Bot [OFF]") >= 0, "a walking player is marked [OFF]");
		}
		g_clients[0].moveType = s2sdk::MoveType::Walk;
	}

	// An empty list shows the disabled NoPlayers entry instead of an empty menu.
	{
		for (FakeClient& client : g_clients) {
			client.connected = false;
			client.inGame = false;
		}
		g_clients[10].connected = true;
		g_clients[10].inGame = true;
		g_clients[10].hasPawn = false; // the admin himself must not be listed
		Check(SelectAdminItem(10, "who"), "who item registered");
		Update();
		const FakeMenu* menu = OpenMenuOf(10);
		Check(menu != nullptr, "the empty player list still opens");
		if (menu != nullptr) {
			const int32_t index = IndexOfItem(*menu, "No available players");
			Check(index >= 0, "an empty list shows the NoPlayers phrase");
			Check(index >= 0 && menu->items[static_cast<std::size_t>(index)].style ==
									 s2sdk::MenuItemStyle::Disabled,
				  "the NoPlayers entry is not selectable");
		}
	}

	FinishTest();
}

// --- D. slot identity / reuse ---------------------------------------------------------------

void TestSlotReuse() {
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	MakeHuman(5, "Alice", 76561198000000001ULL);
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	LatchMap();
	Grant(10, "@admin/kill");
	Grant(10, "@admin/kick");

	// The player leaves and a different player takes the same slot.
	{
		Check(SelectAdminItem(10, "kill"), "kill item");
		Update();
		const FakeMenu* menu = OpenMenuOf(10);
		Check(menu != nullptr, "player list open");
		if (menu != nullptr) {
			SelectByDisplay(10, "Alice");
		}
		// Before the deferred selection runs, slot 5 is reused by somebody else.
		MakeHuman(5, "Mallory", 76561198000000099ULL);
		Update();
		Check(g_forceSuicides.empty(), "a selected player who left is not replaced by the slot's new owner");
		Check(ActionCount("kill") == 0, "no action is sent for the replaced slot");
		Check(OpenMenuOf(10) != nullptr, "the list is rebuilt for what is possible now");
	}

	// The player leaves and the slot is emptied.
	{
		Check(SelectAdminItem(10, "kick"), "kick item");
		Update();
		const FakeMenu* menu = OpenMenuOf(10);
		if (menu != nullptr) {
			SelectByDisplay(10, "Mallory");
		}
		g_clients[5] = FakeClient{};
		Update();
		Check(g_kicks.empty(), "a selection whose target is gone performs no kick");
	}

	// A bot entry only acts while the slot still holds a bot.
	{
		MakeBot(2, "Bot");
		Check(SelectAdminItem(10, "kill"), "kill item for the bot");
		Update();
		const FakeMenu* menu = OpenMenuOf(10);
		Check(menu != nullptr, "bot player list open");
		if (menu != nullptr) {
			SelectByDisplay(10, "Bot");
		}
		MakeHuman(2, "HumanOnBotSlot", 76561198000000077ULL);
		Update();
		Check(g_forceSuicides.empty(), "a bot entry does not hit a human that later took the slot");
	}
	// ... and it does act while the slot still holds the bot.
	{
		MakeBot(2, "Bot2");
		Check(SelectAdminItem(10, "kill"), "kill item for the bot again");
		Update();
		const FakeMenu* menu = OpenMenuOf(10);
		if (menu != nullptr) {
			SelectByDisplay(10, "Bot2");
		}
		Update();
		Check(g_forceSuicides.size() == 1 && g_forceSuicides[0].slot == 2,
			  "the bot entry acts while the slot still holds a bot");
	}

	// The admin's slot is reused before a selection: nothing is executed for the new occupant.
	{
		MakeHuman(5, "Mallory", 76561198000000044ULL);
		MakeHuman(10, "Admin", 76561198000000010ULL);
		Grant(10, "@admin/kill");
		Check(SelectAdminItem(10, "kill"), "kill item before the actor changes");
		Update();
		const FakeMenu* menu = OpenMenuOf(10);
		Check(menu != nullptr, "player list before the actor changes");
		if (menu != nullptr) {
			SelectByDisplay(10, "Mallory");
		}
		// The admin leaves and somebody with the same permission takes the slot.
		MakeHuman(10, "NewAdmin", 76561198000000055ULL);
		Grant(10, "@admin/kill");
		g_forceSuicides.clear();
		g_actions.clear();
		Update();
		Check(g_forceSuicides.empty(), "a selection never acts for the new owner of the admin slot");
		Check(g_actions.empty(), "a selection sends no action for the new owner of the admin slot");
		Check(g_openMenus[10].id == 0, "the stale menu of the old admin is closed");
	}

	// A menu selection made by the old occupant is ignored after the slot changed hands.
	{
		Check(SelectAdminItem(10, "kick"), "kick item before the actor changes again");
		Update();
		const FakeMenu* menu = OpenMenuOf(10);
		Check(menu != nullptr, "player list before the second actor change");
		if (menu != nullptr) {
			menu->handler(menu->id, s2sdk::MenuAction::Select, 10,
						  IndexOfItem(*menu, "Mallory"));
		}
		MakeHuman(10, "YetAnotherAdmin", 76561198000000066ULL);
		Grant(10, "@admin/kick");
		g_kicks.clear();
		Update();
		Check(g_kicks.empty(), "a selection from the previous owner of the slot is ignored");
	}

	// The same SteamID64 in a new connection (new controller generation) is a new identity.
	{
		MakeHuman(5, "Mallory", 76561198000000044ULL);
		MakeHuman(10, "Admin", 76561198000000010ULL);
		Grant(10, "@admin/kill");
		Check(SelectAdminItem(10, "kill"), "kill item before the target reconnects");
		Update();
		const FakeMenu* menu = OpenMenuOf(10);
		Check(menu != nullptr, "player list before the target reconnects");
		if (menu != nullptr) {
			SelectByDisplay(10, "Mallory");
		}
		g_clients[5].generation += 1; // reconnect with the same SteamID64
		g_forceSuicides.clear();
		g_actions.clear();
		Update();
		Check(g_forceSuicides.empty(), "a reconnected target is not acted on from the old menu");
		Check(g_actions.empty(), "no action is sent to a reconnected target");
		Check(OpenMenuOf(10) != nullptr, "the list is rebuilt after the target reconnects");
	}

	// The admin reconnects (same SteamID64, new generation) before the selection runs.
	{
		MakeHuman(5, "Mallory", 76561198000000044ULL);
		MakeHuman(10, "Admin", 76561198000000010ULL);
		Grant(10, "@admin/kill");
		Check(SelectAdminItem(10, "kill"), "kill item before the admin reconnects");
		Update();
		const FakeMenu* menu = OpenMenuOf(10);
		Check(menu != nullptr, "player list before the admin reconnects");
		if (menu != nullptr) {
			SelectByDisplay(10, "Mallory");
		}
		g_clients[10].generation += 1;
		g_forceSuicides.clear();
		g_actions.clear();
		Update();
		Check(g_forceSuicides.empty(), "a selection from the previous admin connection is dropped");
		Check(g_actions.empty(), "no action is sent for the previous admin connection");
		Check(g_openMenus[10].id == 0, "the stale menu of the previous connection is closed");
	}

	// A selection that arrives after the admin's connection changed is ignored outright.
	{
		MakeHuman(5, "Mallory", 76561198000000044ULL);
		MakeHuman(10, "Admin", 76561198000000010ULL);
		Grant(10, "@admin/kill");
		Check(SelectAdminItem(10, "kill"), "kill item before the late selection");
		Update();
		const FakeMenu* menu = OpenMenuOf(10);
		Check(menu != nullptr, "player list before the late selection");
		g_clients[10].generation += 1;
		if (menu != nullptr) {
			menu->handler(menu->id, s2sdk::MenuAction::Select, 10, IndexOfItem(*menu, "Mallory"));
		}
		g_forceSuicides.clear();
		Update();
		Check(g_forceSuicides.empty(), "a late selection from the old connection is ignored");
	}

	// The admin's own slot is gone before the deferred menu opens: nothing is shown.
	{
		MakeHuman(10, "Admin", 76561198000000010ULL);
		Check(SelectAdminItem(10, "kill"), "kill item queued");
		g_clients[10].connected = false;
		g_clients[10].inGame = false;
		// s2sdk's menu manager ends the open menu when the client disconnects.
		FakeCancelClientMenu(10, s2sdk::MenuCancelReason::Disconnect);
		Update();
		Check(g_openMenus[10].id == 0, "a menu is not opened for an admin who left");
		Check(OpenMenuOf(10) == nullptr, "no menu is left open for the departed admin");
	}

	FinishTest();
}

// --- E. the map gate (PITFALLS B1/B2) -------------------------------------------------------

void TestMapGate() {
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	MakeHuman(11, "Target", 76561198000000011ULL);
	Grant(10, "@admin/who");
	Grant(10, "@admin/kill");
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	Check(!g_mapReady, "the gate is shut on start");

	// Before the first map: not one client call reaches s2sdk.
	ResetClientCounters();
	ResetPrints();
	DispatchCommand("mm_who", -1, s2sdk::ConCommandContext::Console, {"mm_who", "Target"});
	DispatchCommand("mm_kill", -1, s2sdk::ConCommandContext::Console, {"mm_kill", "Target"});
	Check(g_clientCalls == 0, "no client call is made before the gate opens");
	Check(g_serverPrints.empty(), "no data is printed before the gate opens");
	Check(SelectAdminItem(10, "kill"), "the admin item callback is registered");
	Update(); // a plugin loaded into a running map: the first update latches readiness
	Check(g_mapReady, "the first update latches the gate");
	// A selection made while the gate was shut cannot be carried over: the admin menu is only
	// reachable once a map is up. Select again now that the latch is open.
	Check(SelectAdminItem(10, "kill"), "the admin item callback is registered after the latch");
	Update();
	Check(OpenMenuOf(10) != nullptr, "the menu is shown after the latch");
	Check(g_clientCalls > 0, "client calls happen after the latch");

	// From OnMapEnd to the next OnMapStart not one client call may happen.
	const s2sdk::MenuId openMenuId = g_openMenus[10].id;
	Check(openMenuId != 0, "a menu is open before the map ends");
	FakeMapEnd();
	Check(!g_mapReady, "OnMapEnd closes the gate");
	Check(g_openMenus[10].id == 0, "OnMapEnd drops the menu bookkeeping");
	Check(g_clientMenu[10] == 0, "OnMapEnd ends the displayed session");
	Check(!g_cancelledMenus.empty() &&
			  g_cancelledMenus.back().second == s2sdk::MenuCancelReason::Disconnect,
		  "a menu still shown at map end is ended with Disconnect (never a client call)");
	ResetClientCounters();
	ResetPrints();
	DispatchCommand("mm_who", -1, s2sdk::ConCommandContext::Console, {"mm_who", "Target"});
	Check(g_clientCalls == 0, "no client call is made between OnMapEnd and OnMapStart");
	Check(g_serverPrints.empty(), "no data is printed between maps");
	Update(); // the deferred destroys run in the map-change window
	Check(g_clientCalls == 0, "the update in the map-change window makes no client call");
	Check(std::find(g_destroyedMenuIds.begin(), g_destroyedMenuIds.end(), openMenuId) !=
			  g_destroyedMenuIds.end(),
		  "the menu handle is destroyed during the map-change window");
	FakeMapStart();
	Check(g_mapReady, "OnMapStart reopens the gate");

	// A plugin loaded into the map-change window must not latch on its first update.
	FinishTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	g_coreLoaded = true;
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start in the map-change window");
	FakeMapEnd();
	Update();
	Check(!g_mapReady, "an OnMapEnd seen since OnPluginStart keeps the gate shut");
	FakeMapStart();
	Check(g_mapReady, "the next OnMapStart opens it");

	FinishTest();
}

// --- F. the menu lifecycle (PITFALLS C10) ---------------------------------------------------

void TestMenuLifecycle() {
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	MakeHuman(11, "Target", 76561198000000011ULL);
	MakeHuman(12, "Other", 76561198000000012ULL);
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	LatchMap();
	Grant(10, "@admin/kill");
	Grant(10, "@admin/changeteam");
	Grant(10, "@admin/switchteam");
	Grant(10, "@admin/slap");
	Grant(10, "@admin/who");

	// MenuId 0 is burnt and the menu is re-created with a usable id.
	{
		Check(SelectAdminItem(10, "kill"), "kill item");
		Update();
		const FakeMenu* menu = OpenMenuOf(10);
		Check(menu != nullptr, "the first menu exists");
		Check(g_menuZeroBurned, "a menu created with id 0 is destroyed and re-created");
		Check(menu != nullptr && menu->id != 0, "the displayed menu does not use id 0");
	}

	// No player choice: pisexcompat's default slot type is HUD_LAYOUT -> hud if the backend is
	// registered, otherwise button (never centerhtml).
	{
		g_clientMenuTypes[10].clear();
		g_menuTypes.erase("hud");
		Check(SelectAdminItem(10, "kill"), "kill item without hud");
		Update();
		const FakeMenu* menu = OpenMenuOf(10);
		Check(menu != nullptr && menu->type == "button",
			  "the Pisex HUD_LAYOUT default is button while the hud backend is missing");
		g_menuTypes.insert("hud");
		Check(SelectAdminItem(10, "kill"), "kill item with hud");
		Update();
		menu = OpenMenuOf(10);
		Check(menu != nullptr && menu->type == "hud", "the hud backend is used when registered");
		g_menuTypes.erase("hud");
	}

	// The player's own "!menu" choice: the known builtins pass through, "hud"/"wasd" only while
	// the matching backend is registered (otherwise button, even when the other one is there);
	// an unknown answer falls through to the mapped HUD_LAYOUT default.
	{
		g_menuTypes.insert("wasd"); // registered, but hud is not
		g_clientMenuTypes[10] = "chat";
		Check(SelectAdminItem(10, "kill"), "kill item with the chat choice");
		Update();
		FakeMenu const* menu = OpenMenuOf(10);
		Check(menu != nullptr && menu->type == "chat", "the chat choice is kept");
		g_clientMenuTypes[10] = "centerhtml";
		Check(SelectAdminItem(10, "kill"), "kill item with the centerhtml choice");
		Update();
		menu = OpenMenuOf(10);
		Check(menu != nullptr && menu->type == "centerhtml", "the centerhtml choice is kept");
		g_clientMenuTypes[10] = "button";
		Check(SelectAdminItem(10, "kill"), "kill item with the button choice");
		Update();
		menu = OpenMenuOf(10);
		Check(menu != nullptr && menu->type == "button", "the button choice is kept");

		g_clientMenuTypes[10] = "wasd";
		Check(SelectAdminItem(10, "kill"), "kill item with the wasd choice");
		Update();
		menu = OpenMenuOf(10);
		Check(menu != nullptr && menu->type == "wasd", "the wasd choice uses its backend");
		g_menuTypes.erase("wasd");
		Check(SelectAdminItem(10, "kill"), "kill item with a wasd choice without its backend");
		Update();
		menu = OpenMenuOf(10);
		Check(menu != nullptr && menu->type == "button",
			  "a wasd choice without its backend is button (not hud/centerhtml)");

		g_menuTypes.insert("hud");
		g_clientMenuTypes[10] = "hud";
		Check(SelectAdminItem(10, "kill"), "kill item with the hud choice");
		Update();
		menu = OpenMenuOf(10);
		Check(menu != nullptr && menu->type == "hud", "the hud choice uses its backend");
		g_menuTypes.erase("hud");
		g_menuTypes.insert("wasd");
		Check(SelectAdminItem(10, "kill"), "kill item with a hud choice without its backend");
		Update();
		menu = OpenMenuOf(10);
		Check(menu != nullptr && menu->type == "button",
			  "a hud choice without its backend is button (not the registered wasd)");
		g_menuTypes.insert("hud");

		g_clientMenuTypes[10] = "mystery";
		Check(SelectAdminItem(10, "kill"), "kill item with an unknown hudmenu answer");
		Update();
		menu = OpenMenuOf(10);
		Check(menu != nullptr && menu->type == "hud",
			  "an unknown answer falls through to the mapped HUD_LAYOUT type");

		// hudmenu not installed at all: the mapped HUD_LAYOUT default is used.
		__hudmenu_GetClientMenuType = nullptr;
		Check(SelectAdminItem(10, "kill"), "kill item without the hudmenu API");
		Update();
		menu = OpenMenuOf(10);
		Check(menu != nullptr && menu->type == "hud", "the mapped hud default works without hudmenu");
		g_menuTypes.erase("hud");
		Check(SelectAdminItem(10, "kill"), "kill item without hudmenu and hud");
		Update();
		menu = OpenMenuOf(10);
		Check(menu != nullptr && menu->type == "button",
			  "without hudmenu the mapped default is hud if registered, otherwise button");
		InstallFakes(); // restore the optional API for the rest of the test
		g_menuTypes.insert("hud");
	}

	// A selection performs no engine action inside the handler; the update does.
	{
		const FakeMenu* menu = OpenMenuOf(10);
		Check(menu != nullptr, "player list open");
		if (menu != nullptr) {
			SelectByDisplay(10, "Target");
		}
		Check(g_forceSuicides.empty(), "no engine action runs inside the menu handler");
		const s2sdk::MenuId before = g_clientMenu[10];
		Update();
		Check(g_forceSuicides.size() == 1, "the deferred selection kills the target on the update");
		Check(g_clientMenu[10] != 0 && g_clientMenu[10] != before,
			  "the list is replaced by a fresh one after the action");
	}

	// ExitBack of the player list goes back to the admin core's last category.
	{
		ResetPrints();
		FakeCancelClientMenu(10, s2sdk::MenuCancelReason::ExitBack);
		Update();
		Check(g_lastCategoryCalls.size() == 1 && g_lastCategoryCalls[0] == 10,
			  "the player list's back button reopens the admin category");
	}

	// Timeout and exit just destroy the menu.
	{
		Check(SelectAdminItem(10, "changeteam"), "changeteam item");
		Update();
		const FakeMenu* menu = OpenMenuOf(10);
		Check(menu != nullptr, "player list for changeteam");
		if (menu != nullptr) {
			SelectByDisplay(10, "Other");
		}
		Update(); // opens the team picker
		menu = OpenMenuOf(10);
		Check(menu != nullptr, "the team picker opened");
		Check(g_teamChanges.empty(), "opening the team picker changes nobody's team yet");
		if (menu != nullptr) {
			SelectByDisplay(10, "T");
		}
		Update();
		Check(g_teamChanges.size() == 1 && g_teamChanges[0].number == 2,
			  "the team picker changes the team outside the handler");
		// The team picker is re-shown after the change (the original kept it open).
		menu = OpenMenuOf(10);
		Check(menu != nullptr, "the team picker is re-shown after the change");
		if (menu != nullptr) {
			const int destroyedBefore = g_destroyMenuCalls;
			FakeCancelClientMenu(10, s2sdk::MenuCancelReason::Timeout);
			Update();
			Check(g_destroyMenuCalls > destroyedBefore, "a timed-out menu is destroyed");
			Check(OpenMenuOf(10) == nullptr, "a timed-out menu is not re-shown");
			Check(g_lastCategoryCalls.size() == 1, "a timeout does not go back to the admin menu");
		}
	}

	// The team picker's back button returns to the same item's player list.
	{
		Check(SelectAdminItem(10, "switchteam"), "switchteam item");
		Update();
		const FakeMenu* menu = OpenMenuOf(10);
		if (menu != nullptr) {
			SelectByDisplay(10, "Other");
		}
		Update(); // team picker
		menu = OpenMenuOf(10);
		Check(menu != nullptr, "the switch-team picker opened");
		const std::size_t teamChangesBefore = g_teamChanges.size();
		FakeCancelClientMenu(10, s2sdk::MenuCancelReason::ExitBack);
		Update();
		menu = OpenMenuOf(10);
		Check(menu != nullptr && menu->title == "Select player",
			  "the team picker's back button returns to the player list");
		Check(g_teamChanges.size() == teamChangesBefore, "backing out of the team picker changes no team");
	}

	// Slap opens the damage picker and applies the chosen damage.
	{
		Check(SelectAdminItem(10, "slap"), "slap item");
		Update();
		const FakeMenu* menu = OpenMenuOf(10);
		if (menu != nullptr) {
			SelectByDisplay(10, "Other");
		}
		Update(); // damage picker
		menu = OpenMenuOf(10);
		Check(menu != nullptr && menu->title == "Select damage amount", "the damage picker opened");
		Check(IndexOfItem(*menu, "50") >= 0, "the damage picker offers the original damage values");
		if (menu != nullptr) {
			// "50" is the sixth item: page 2, first row (the chat backend pages by five).
			g_clientOffset[10] = 5;
			SelectMenuItemFake(10, 5);
			g_clientOffset[10] = 0;
		}
		Update();
		Check(g_clients[12].health == 50, "the chosen slap damage is applied");
		Check(ActionCount("slap") == 1 && LastAction()->param == "12 50",
			  "the slap action carries the target and the damage");
	}

	// A digit of a not-displayed page is ignored (pisexcompat's guard).
	{
		for (int32_t i = 20; i < 28; ++i) {
			MakeHuman(i, "P" + std::to_string(i), 76561198000000200ULL + static_cast<uint64_t>(i));
		}
		Check(SelectAdminItem(10, "who"), "who item with a full list");
		Update();
		const FakeMenu* menu = OpenMenuOf(10);
		Check(menu != nullptr, "full player list open");
		Check(menu != nullptr && menu->items.size() >= 8, "the list has more than one page");
		const s2sdk::MenuId menuId = menu != nullptr ? menu->id : 0;
		// The chat backend maps digit 7 to absolute index 6 even though page 0 shows 0..4.
		if (menu != nullptr) {
			SelectMenuItemFake(10, 6);
		}
		Update();
		Check(g_openMenus[10].id == menuId, "a hidden-page digit does not replace the menu");
	}

	// A menu whose display is refused is destroyed immediately and leaves no state behind.
	{
		__s2sdk_DisplayMenu = &FakeDisplayMenuFail;
		const int createdBefore = g_createMenuCalls;
		Check(SelectAdminItem(10, "kill"), "kill item with a refused display");
		Update();
		Check(g_createMenuCalls > createdBefore, "the menu was created before the refused display");
		Check(g_openMenus[10].id == 0, "a menu that could not be displayed leaves no state");
		Check(g_createMenuCalls == g_destroyMenuCalls,
			  "every created menu (including the burnt id 0) is destroyed when the display fails");
		Check(g_clientMenu[10] == 0, "no menu is left displayed for the slot");
		InstallFakes(); // restore the display export
	}

	FinishTest();
}

// --- G. the admin item callbacks are retired on unload (PITFALLS A7) --------------------------

void TestAdminItemRetirement() {
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	LatchMap();
	Check(g_adminMenuItemsRegistered, "the module records that it registered its items");

	// The admin core holds the eight raw trampolines while the module is loaded.
	int withSelect = 0;
	int withDisplay = 0;
	for (const ItemDef& def : kItems) {
		const FakeItem* item = FindFakeItem(def.szIdentity);
		Check(item != nullptr, std::string("item ") + def.szIdentity + " is registered");
		if (item != nullptr) {
			withSelect += item->select != nullptr ? 1 : 0;
			withDisplay += item->display != nullptr ? 1 : 0;
			Check(item->category == "players", "the item stays in the players category");
			Check(item->flags == def.szPermission, "the item keeps its permission flags");
		}
	}
	Check(withSelect == 8 && withDisplay == 8,
		  "all eight items hold the module's callbacks while it is loaded");
	Check(!g_categories.empty() && g_categories.front().display == nullptr,
		  "the category is registered with a null display callback (no trampoline in the core)");

	StopPlugin();
	for (const ItemDef& def : kItems) {
		const FakeItem* item = FindFakeItem(def.szIdentity);
		Check(item != nullptr, std::string("item ") + def.szIdentity + " still exists after unload");
		if (item != nullptr) {
			Check(item->select == nullptr,
				  std::string("the select callback of ") + def.szIdentity + " is null after unload");
			Check(item->display == nullptr,
				  std::string("the display callback of ") + def.szIdentity + " is null after unload");
		}
	}
	Check(!g_adminMenuItemsRegistered, "the module no longer claims the registration is live");

	// A second unload must not re-register anything (idempotent).
	StopPlugin();

	// A reload registers the real callbacks again and the items work.
	MakeHuman(10, "Admin", 76561198000000010ULL);
	MakeHuman(11, "Target", 76561198000000011ULL);
	Grant(10, "@admin/kill");
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "reload after the retirement");
	LatchMap();
	int restored = 0;
	for (const ItemDef& def : kItems) {
		const FakeItem* item = FindFakeItem(def.szIdentity);
		if (item != nullptr && item->select != nullptr && item->display != nullptr) {
			++restored;
		}
	}
	Check(restored == 8, "the reload restores all eight callbacks");
	Check(SelectAdminItem(10, "kill"), "the reloaded kill item is selectable");
	Update();
	Check(OpenMenuOf(10) != nullptr, "the reloaded item opens the player list");
	FinishTest();
}

// A missing RegisterItem binding at unload is fail-soft: nothing crashes, the stale pointers
// simply cannot be cleared (plugify unloads the consumer before its dependency).
void TestAdminItemRetirementFailSoft() {
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	LatchMap();
	__admin_system_RegisterItem = nullptr;
	StopPlugin(); // must not crash and must not touch any callback
	Check(!g_adminMenuItemsRegistered, "the fail-soft path still clears the live flag");
	FinishTest();
}

// --- H. the Pisex menu-type table -----------------------------------------------------------

// The exact pisexcompat table (libs/pisexcompat/src/menu.cpp): the hudmenu answer first, then the
// slot's Pisex MenuType. All choices against all backend combinations, plus unknown/empty.
void TestMenuTypeResolution() {
	struct ChoiceCase {
		const char* choice;
		bool hud;
		bool wasd;
		const char* expected;
	};
	const ChoiceCase cases[] = {
		// Pisex default (slot type HUD_LAYOUT), no player choice (also the null-binding path).
		{"", false, false, "button"},
		{"", true, false, "hud"},
		{"", false, true, "button"},
		{"", true, true, "hud"},
		// Known builtins pass through regardless of the custom backends.
		{"chat", false, false, "chat"},
		{"chat", true, true, "chat"},
		{"centerhtml", false, false, "centerhtml"},
		{"centerhtml", true, true, "centerhtml"},
		{"button", false, false, "button"},
		{"button", true, true, "button"},
		// "hud" only with its backend, otherwise button even when wasd is registered.
		{"hud", false, false, "button"},
		{"hud", true, false, "hud"},
		{"hud", false, true, "button"},
		{"hud", true, true, "hud"},
		// "wasd" only with its backend, otherwise button even when hud is registered.
		{"wasd", false, false, "button"},
		{"wasd", true, false, "button"},
		{"wasd", false, true, "wasd"},
		{"wasd", true, true, "wasd"},
		// Unknown/foreign answers fall through to the mapped HUD_LAYOUT default.
		{"mystery", false, false, "button"},
		{"mystery", true, false, "hud"},
		{"HUD", false, true, "button"},
		{"HUD", true, true, "hud"},
	};
	for (const ChoiceCase& test : cases) {
		const std::string actual = ResolveMenuTypeName(test.choice, PisexMenuType::HudLayout,
													   test.hud, test.wasd);
		Check(actual == test.expected,
			  std::string("choice '") + test.choice + "' hud=" + (test.hud ? "1" : "0") +
				  " wasd=" + (test.wasd ? "1" : "0") + " -> " + test.expected + " (got " + actual +
				  ")");
	}

	// The mapped Pisex types (the module never forces a slot type, but the table is exact).
	for (const bool hud : {false, true}) {
		for (const bool wasd : {false, true}) {
			Check(MappedMenuTypeName(PisexMenuType::Chat, hud, wasd) == "chat",
				  "Pisex CHAT maps to chat");
			Check(MappedMenuTypeName(PisexMenuType::Center, hud, wasd) == "centerhtml",
				  "Pisex CENTER maps to centerhtml");
			Check(MappedMenuTypeName(PisexMenuType::CenterWasd, hud, wasd) ==
					  (wasd ? "wasd" : "button"),
				  "Pisex CENTER_WASD maps to wasd when registered, otherwise button");
			Check(MappedMenuTypeName(PisexMenuType::HudLayout, hud, wasd) ==
					  (hud ? "hud" : "button"),
				  "Pisex HUD_LAYOUT maps to hud when registered, otherwise button");
		}
	}
}

// --- I. safe menu destruction (PITFALLS B2 + A7) ---------------------------------------------

// An active menu, a map-down window and a manager that refuses CancelClientMenu: the plugin must
// re-query the display, switch the menu to a builtin no-op-close type and only then DestroyMenu -
// no backend close and no callback may reach the torn-down client layer.
void TestMenuCleanupFailSafe() {
	// 1. The map ends while the display survives the cancel; the update performs the switch.
	{
		PrepareTest();
		MakeHuman(10, "Admin", 76561198000000010ULL);
		Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
		LatchMap();
		Grant(10, "@admin/kill");
		Check(SelectAdminItem(10, "kill"), "kill item");
		Update();
		const FakeMenu* menu = OpenMenuOf(10);
		Check(menu != nullptr, "the menu is displayed");
		const s2sdk::MenuId id = menu != nullptr ? menu->id : 0;
		Check(menu != nullptr && menu->type != "chat" && menu->type != "console",
			  "the displayed menu uses a backend that would close on the client");

		g_cancelFails = true;
		g_liveBackendCloses = 0;
		g_backendCloseTypes.clear();
		g_cancelAttempts.clear();
		g_setMenuTypeCalls = 0;
		g_logs.clear();
		FakeMapEnd();
		Check(g_clientMenu[10] == id, "the fake cancel left the display active through OnMapEnd");
		Check(!g_cancelAttempts.empty(), "OnMapEnd tried to end the display with Disconnect");
		Update(); // the deferred destroy runs in the map-change window
		Check(g_liveBackendCloses == 0, "no backend close touched the torn-down client layer");
		Check(FindMenu(id) == nullptr, "the menu is destroyed after the no-op type switch");
		Check(g_setMenuTypeCalls > 0, "SetMenuType was used to make the close a no-op");
		Check(g_backendCloseTypes.empty(), "the backend close stayed a no-op");
		Check(!HasLog("A7_UNRESOLVED"), "the cleanup is fully resolved");
		const int handlersAfter = g_handlerCalls;
		Check(!FakeCancelClientMenu(10, s2sdk::MenuCancelReason::Timeout),
			  "no display is left to call the plugin's handler");
		Check(g_handlerCalls == handlersAfter, "no live callback runs after the destroy");
		FinishTest();
	}

	// 2. The plugin unloads with the map down while the display survives the cancel.
	{
		PrepareTest();
		MakeHuman(10, "Admin", 76561198000000010ULL);
		Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
		LatchMap();
		Grant(10, "@admin/kill");
		Check(SelectAdminItem(10, "kill"), "kill item");
		Update();
		const FakeMenu* menu = OpenMenuOf(10);
		Check(menu != nullptr, "the menu is displayed");
		const s2sdk::MenuId id = menu != nullptr ? menu->id : 0;

		g_cancelFails = true;
		g_liveBackendCloses = 0;
		g_setMenuTypeCalls = 0;
		g_logs.clear();
		g_mapReady = false; // the map is already torn down, no OnMapEnd bookkeeping was run
		StopPlugin();
		Check(g_liveBackendCloses == 0, "the unload makes no backend close on a torn-down client");
		Check(FindMenu(id) == nullptr, "the unload destroys the menu after the no-op type switch");
		Check(g_clientMenu[10] == 0, "no display is left behind by the unload");
		Check(g_setMenuTypeCalls > 0, "the unload switched the type before destroying");
		Check(!HasLog("A7_UNRESOLVED"), "the unload cleanup is fully resolved");
		const int handlersAfter = g_handlerCalls;
		Check(!FakeCancelClientMenu(10, s2sdk::MenuCancelReason::Timeout),
			  "the unloaded plugin left no display (and no live callback) behind");
		Check(g_handlerCalls == handlersAfter, "no callback runs into the unloaded plugin");
		FinishTest();
	}

	// 2a. The exact requested path: active -> OnMapEnd (cancel refused, display survives) ->
	//     unload without an update in between.
	{
		PrepareTest();
		MakeHuman(10, "Admin", 76561198000000010ULL);
		Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
		LatchMap();
		Grant(10, "@admin/kill");
		Check(SelectAdminItem(10, "kill"), "kill item");
		Update();
		const FakeMenu* menu = OpenMenuOf(10);
		Check(menu != nullptr, "the menu is displayed");
		const s2sdk::MenuId id = menu != nullptr ? menu->id : 0;

		g_cancelFails = true;
		g_liveBackendCloses = 0;
		g_setMenuTypeCalls = 0;
		g_logs.clear();
		FakeMapEnd();
		Check(g_clientMenu[10] == id, "OnMapEnd could not end the display (cancel refused)");
		Check(g_openMenus[10].id == 0, "the handle is queued for the deferred destroy");
		StopPlugin(); // the unload must run the queued safe destroy itself
		Check(g_liveBackendCloses == 0, "active->MapEnd->unload makes no backend close");
		Check(FindMenu(id) == nullptr, "active->MapEnd->unload destroys the menu safely");
		Check(g_clientMenu[10] == 0, "no display is left after active->MapEnd->unload");
		Check(g_setMenuTypeCalls > 0, "the queued handle is switched to a no-op type at unload");
		Check(!HasLog("A7_UNRESOLVED"), "active->MapEnd->unload is fully resolved");
		const int handlersAfter = g_handlerCalls;
		Check(!FakeCancelClientMenu(10, s2sdk::MenuCancelReason::Timeout),
			  "no display is left for a late cancel");
		Check(g_handlerCalls == handlersAfter, "no live callback runs after the unload");
		FinishTest();
	}

	// 2b. A manager that answers true but leaves the display active is still handled safely.
	{
		PrepareTest();
		MakeHuman(10, "Admin", 76561198000000010ULL);
		Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
		LatchMap();
		Grant(10, "@admin/kill");
		Check(SelectAdminItem(10, "kill"), "kill item");
		Update();
		const FakeMenu* menu = OpenMenuOf(10);
		Check(menu != nullptr, "the menu is displayed");
		const s2sdk::MenuId id = menu != nullptr ? menu->id : 0;

		g_cancelLies = true;
		g_liveBackendCloses = 0;
		g_logs.clear();
		FakeMapEnd();
		Check(g_clientMenu[10] == id, "the lying cancel left the display active");
		Update();
		Check(g_liveBackendCloses == 0, "the re-query still avoids the backend close");
		Check(FindMenu(id) == nullptr, "the lying cancel is followed by the no-op type switch");
		Check(!HasLog("A7_UNRESOLVED"), "the lying-cancel cleanup is fully resolved");
		g_cancelLies = false;
		FinishTest();
	}

	// 3. The switch itself is impossible: the request is parked, A7_UNRESOLVED is explicit, and a
	//    later map start can destroy the menu safely.
	{
		PrepareTest();
		MakeHuman(10, "Admin", 76561198000000010ULL);
		Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
		LatchMap();
		Grant(10, "@admin/kill");
		Check(SelectAdminItem(10, "kill"), "kill item");
		Update();
		const FakeMenu* menu = OpenMenuOf(10);
		Check(menu != nullptr, "the menu is displayed");
		const s2sdk::MenuId id = menu != nullptr ? menu->id : 0;

		g_cancelFails = true;
		g_setMenuTypeFails = true;
		g_liveBackendCloses = 0;
		g_logs.clear();
		FakeMapEnd();
		Update();
		Check(HasLog("A7_UNRESOLVED"), "an impossible switch is reported explicitly (A7_UNRESOLVED)");
		Check(FindMenu(id) != nullptr, "the menu is not blindly destroyed while displayed");
		Check(g_clientMenu[10] == id, "the display is still active");
		Check(g_liveBackendCloses == 0, "no backend close happened on the torn-down client");
		Check(!g_destroyQueue.empty(), "the destroy request is parked for a later retry");

		// The next map makes the ordinary cancel+destroy path safe again.
		g_cancelFails = false;
		g_setMenuTypeFails = false;
		FakeMapStart();
		g_logs.clear();
		Update();
		Check(FindMenu(id) == nullptr, "the parked menu is destroyed once the map is back");
		Check(g_destroyQueue.empty(), "the parked request is gone after the successful destroy");
		Check(!HasLog("A7_UNRESOLVED"), "no unresolved cleanup remains after the retry");
		FinishTest();
	}

	// 4. The plugin unloads while even the no-op switch is impossible: log A7_UNRESOLVED instead
	//    of pretending the handle was released.
	{
		PrepareTest();
		MakeHuman(10, "Admin", 76561198000000010ULL);
		Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
		LatchMap();
		Grant(10, "@admin/kill");
		Check(SelectAdminItem(10, "kill"), "kill item");
		Update();
		const FakeMenu* menu = OpenMenuOf(10);
		Check(menu != nullptr, "the menu is displayed");
		const s2sdk::MenuId id = menu != nullptr ? menu->id : 0;

		g_cancelFails = true;
		g_setMenuTypeFails = true;
		g_liveBackendCloses = 0;
		g_logs.clear();
		g_mapReady = false;
		StopPlugin();
		Check(HasLog("A7_UNRESOLVED"), "an unreleasable menu is reported at unload (A7_UNRESOLVED)");
		Check(FindMenu(id) != nullptr, "the unreleasable handle is not silently forgotten");
		Check(g_liveBackendCloses == 0, "no backend close was attempted on the torn-down client");
		Check(g_openMenus[10].id == 0, "the plugin no longer tracks the handle after the unload");
		FinishTest();
	}
}

// --- I. paired bindings and registration (PITFALLS A7) ---------------------------------------

// A registration whose removal half is missing must not happen at all: otherwise the callback
// stays inside the unloaded library. Commands need Add + both remove paths (ownership is known
// only after the Add call); the map listeners need both register *and* both unregister halves;
// the admin core listener needs its Register/Unregister pair.
void TestPairedBindingRegistration() {
	// Commands: no AddConsoleCommand call at all without either removal path.
	{
		struct Case {
			const char* what;
			void (*drop)();
		};
		const Case cases[] = {
			{"RemoveCommand", [] { __s2sdk_RemoveCommand = nullptr; }},
			{"RemoveCommandListener", [] { __s2sdk_RemoveCommandListener = nullptr; }},
		};
		for (const Case& test : cases) {
			PrepareTest();
			MakeHuman(10, "Admin", 76561198000000010ULL);
			test.drop();
			Check(StartPlugin().GetCode() == plg::PluginCode::Ok,
				  std::string("start without ") + test.what);
			LatchMap();
			Check(g_addedCommands.empty(),
				  std::string("no command is registered without ") + test.what +
					  " (the callback could not be removed)");
			Check(g_items.size() == 8, "the unrelated admin items are unaffected");
			StopPlugin();
			Check(g_removedCommands.empty(), "nothing to remove after the skipped registration");
			FinishTest();
		}
	}

	// Map listeners: all four bindings are required before the first registration.
	{
		struct Case {
			const char* what;
			void (*drop)();
		};
		const Case cases[] = {
			{"OnMapStart_Register", [] { __s2sdk_OnMapStart_Register = nullptr; }},
			{"OnMapStart_Unregister", [] { __s2sdk_OnMapStart_Unregister = nullptr; }},
			{"OnMapEnd_Register", [] { __s2sdk_OnMapEnd_Register = nullptr; }},
			{"OnMapEnd_Unregister", [] { __s2sdk_OnMapEnd_Unregister = nullptr; }},
		};
		for (const Case& test : cases) {
			PrepareTest();
			MakeHuman(10, "Admin", 76561198000000010ULL);
			test.drop();
			Check(StartPlugin().GetCode() == plg::PluginCode::Ok,
				  std::string("start without ") + test.what);
			LatchMap();
			Check(g_mapStartRegisters == 0 && g_mapEndRegisters == 0,
				  std::string("no map listener is registered without ") + test.what);
			Check(!g_mapReady, "the map gate stays shut without the listener pair");
			StopPlugin();
			Check(g_mapStartUnregisters == 0 && g_mapEndUnregisters == 0,
				  "no unregister runs for a listener that was never registered");
			Check(g_addedCommands.size() == 8, "the unrelated commands are unaffected");
			FinishTest();
		}
	}

	// The admin core listener: both halves of the pair are required before registering.
	{
		struct Case {
			const char* what;
			void (*drop)();
		};
		const Case cases[] = {
			{"OnCoreLoaded_Register", [] { __admin_system_OnCoreLoaded_Register = nullptr; }},
			{"OnCoreLoaded_Unregister", [] { __admin_system_OnCoreLoaded_Unregister = nullptr; }},
		};
		for (const Case& test : cases) {
			PrepareTest();
			MakeHuman(10, "Admin", 76561198000000010ULL);
			test.drop();
			Check(StartPlugin().GetCode() == plg::PluginCode::Ok,
				  std::string("start without ") + test.what);
			LatchMap();
			Check(g_coreLoadedRegisters == 0,
				  std::string("no admin core listener is registered without ") + test.what);
			Check(g_items.empty(), "no admin items are registered without the paired listener");
			Check(g_addedCommands.size() == 8, "the commands are still registered");
			StopPlugin(); // must not call the missing unregister half
			Check(g_coreLoadedUnregisters == 0,
				  "no unregister runs for a listener that was never registered");
			FinishTest();
		}
	}
}

// --- J. missing core/bindings (PITFALLS A1/A4) ----------------------------------------------

void TestMissingBindings() {
	// Everything null: the plugin still starts, updates and unloads.
	{
		PrepareTest();
		__s2sdk_OnMapStart_Register = nullptr;
		__s2sdk_OnMapEnd_Register = nullptr;
		__s2sdk_AddConsoleCommand = nullptr;
		__s2sdk_RemoveCommand = nullptr;
		__s2sdk_CreateMenu = nullptr;
		__admin_system_GetPluginVersion = nullptr;
		__admin_system_HasPermission = nullptr;
		__admin_system_OnCoreLoaded_Register = nullptr;
		Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start with every binding null");
		Update();
		Check(!g_mapReady, "without map listeners the gate stays shut");
		StopPlugin();
		Check(g_addedCommands.empty(), "no command is registered without AddConsoleCommand");
		FinishTest();
	}

	// The admin core is missing: commands work but only for the server console, no menu items.
	{
		PrepareTest();
		MakeHuman(10, "Admin", 76561198000000010ULL);
		MakeHuman(11, "Target", 76561198000000011ULL);
		__admin_system_GetPluginVersion = nullptr;
		__admin_system_HasPermission = nullptr;
		__admin_system_RegisterItem = nullptr;
		Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start without admin_system bindings");
		LatchMap();
		Check(g_addedCommands.size() == 8, "the commands are still registered");
		Check(g_items.empty(), "no admin menu items are registered without the core");
		ResetPrints();
		DispatchCommand("mm_kill", 10, s2sdk::ConCommandContext::Console, {"mm_kill", "Target"});
		Check(g_forceSuicides.empty(), "a client caller cannot bypass a missing permission check");
		DispatchCommand("mm_kill", -1, s2sdk::ConCommandContext::Console, {"mm_kill", "Target"});
		Check(g_forceSuicides.size() == 1, "the server console still works without the core");
		ResetPrints();
		DispatchCommand("mm_who", -1, s2sdk::ConCommandContext::Console, {"mm_who", "Admin"});
		Check(g_serverPrints.empty(), "mm_who shows no admin data when the core is missing");
		StopPlugin();
		FinishTest();
	}

	// The readiness export is missing (admin_system < 1.1.0): client calls fail closed.
	{
		PrepareTest();
		MakeHuman(10, "Admin", 76561198000000010ULL);
		MakeHuman(11, "Target", 76561198000000011ULL);
		Grant(10, "@admin/kill");
		Grant(10, "@admin/who");
		__admin_system_IsClientAdminDataReady = nullptr;
		Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start without the readiness export");
		LatchMap();
		ResetPrints();
		DispatchCommand("mm_kill", 10, s2sdk::ConCommandContext::Console, {"mm_kill", "Target"});
		Check(g_forceSuicides.empty(),
			  "a client call is denied when the readiness export is missing (fail closed)");
		DispatchCommand("mm_kill", -1, s2sdk::ConCommandContext::Console, {"mm_kill", "Target"});
		Check(g_forceSuicides.size() == 1, "the server console still works without the export");
		ResetPrints();
		DispatchCommand("mm_who", -1, s2sdk::ConCommandContext::Console, {"mm_who", "Admin"});
		Check(g_serverPrints.empty(), "mm_who refuses to show data without the readiness export");
		StopPlugin();
		FinishTest();
	}

	// The s2sdk menu bindings are missing: no menu items, commands still work.
	{
		PrepareTest();
		MakeHuman(10, "Admin", 76561198000000010ULL);
		MakeHuman(11, "Target", 76561198000000011ULL);
		__s2sdk_CreateMenu = nullptr;
		__s2sdk_DisplayMenu = nullptr;
		Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start without the menu API");
		LatchMap();
		Check(g_items.empty(), "the admin menu items are not registered without the menu API");
		Check(g_categories.empty(), "the category is not registered either");
		ResetPrints();
		DispatchCommand("mm_kick", -1, s2sdk::ConCommandContext::Console, {"mm_kick", "Target"});
		Check(g_kicks.size() == 1, "a command still works without the menu API");
		StopPlugin();
		FinishTest();
	}

	// The handle-management menu exports are required before anything is created (incl. id 0):
	// a missing DestroyMenu/GetClientMenu/CancelClientMenu disables the whole menu layer.
	{
		PrepareTest();
		MakeHuman(10, "Admin", 76561198000000010ULL);
		__s2sdk_DestroyMenu = nullptr;
		Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start without DestroyMenu");
		LatchMap();
		Check(g_items.empty(), "no items without DestroyMenu (a handle could never be freed)");
		Check(g_createMenuCalls == 0, "not even the dummy menu id 0 is created without DestroyMenu");
		StopPlugin();
		FinishTest();

		PrepareTest();
		MakeHuman(10, "Admin", 76561198000000010ULL);
		__s2sdk_CancelClientMenu = nullptr;
		Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start without CancelClientMenu");
		LatchMap();
		Check(g_items.empty(), "no items without CancelClientMenu (a display could not be ended)");
		StopPlugin();
		FinishTest();

		PrepareTest();
		MakeHuman(10, "Admin", 76561198000000010ULL);
		__s2sdk_GetClientMenu = nullptr;
		Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start without GetClientMenu");
		LatchMap();
		Check(g_items.empty(), "no items without GetClientMenu");
		StopPlugin();
		FinishTest();

		PrepareTest();
		MakeHuman(10, "Admin", 76561198000000010ULL);
		__s2sdk_SetMenuType = nullptr;
		Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start without SetMenuType");
		LatchMap();
		Check(g_items.empty(),
			  "no items without SetMenuType (the safe-destroy fail-safe could not work)");
		StopPlugin();
		FinishTest();

		PrepareTest();
		MakeHuman(10, "Admin", 76561198000000010ULL);
		__s2sdk_GetMenuType = nullptr;
		Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start without GetMenuType");
		LatchMap();
		Check(g_items.empty(),
			  "no items without GetMenuType (the safe-destroy fail-safe could not verify)");
		StopPlugin();
		FinishTest();
	}

	// The core loads after the plugin: the items are registered from OnCoreLoaded once.
	{
		PrepareTest();
		g_coreLoaded = false;
		MakeHuman(10, "Admin", 76561198000000010ULL);
		Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start before the core");
		Check(g_items.empty(), "no item before the core signalled that it is loaded");
		Check(g_coreLoadedCallback != nullptr, "the core-loaded callback is registered");
		g_coreLoaded = true;
		if (g_coreLoadedCallback != nullptr) {
			g_coreLoadedCallback();
		}
		Check(g_items.size() == 8, "the eight items are registered when the core loads");
		Check(HasCategory("players"), "the category is registered when the core loads");
		StopPlugin();
		FinishTest();
	}

	// A late load into a core that is already up registers the items immediately.
	{
		PrepareTest();
		g_coreLoaded = true;
		MakeHuman(10, "Admin", 76561198000000010ULL);
		Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start after the core");
		Check(g_items.size() == 8, "a plugin loaded after the core registers its items at once");
		Check(g_coreLoadedRegisters == 1, "the core-loaded listener is still registered for a reload");
		StopPlugin();
		FinishTest();
	}
}

// --- H. output and phrases ------------------------------------------------------------------

void TestOutputAndPhrases() {
	PrepareTest();
	MakeHuman(10, "Root", 76561198000000010ULL);
	MakeHuman(11, "Tar%get", 76561198000000011ULL);
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	LatchMap();
	Grant(10, "@admin/kill");
	Grant(10, "@admin/who");

	// Chat is colourised, the server console has its tags stripped.
	{
		g_messageType = 2;
		ResetPrints();
		DispatchCommand("mm_kill", -1, s2sdk::ConCommandContext::Console, {"mm_kill", "11"});
		Check(HasChatAllPrint("\x0F") && !HasChatAllPrint("{LIGHTRED}"),
			  "the chat message carries the control byte, not the tag");
		Check(HasChatAllPrint("Console") && HasChatAllPrint("Tar%get"),
			  "the console actor and the target names are in the broadcast");
	}

	// Message type 1 omits the admin name, type 0 prints nothing.
	{
		g_messageType = 1;
		ResetPrints();
		MakeHuman(11, "Tar%get", 76561198000000011ULL); // alive again
		DispatchCommand("mm_kill", -1, s2sdk::ConCommandContext::Console, {"mm_kill", "11"});
		Check(HasChatAllPrint("Tar%get"), "message type 1 broadcasts the target");
		Check(!HasChatAllPrint("Console"), "message type 1 does not name the actor");
		g_messageType = 0;
		ResetPrints();
		MakeHuman(11, "Tar%get", 76561198000000011ULL);
		DispatchCommand("mm_kill", -1, s2sdk::ConCommandContext::Console, {"mm_kill", "11"});
		Check(g_chatAllPrints.empty(), "message type 0 broadcasts nothing");
	}

	// A '%' in a player name is data, not a format string.
	{
		g_messageType = 1;
		ResetPrints();
		MakeHuman(11, "100%Player", 76561198000000011ULL);
		DispatchCommand("mm_kill", -1, s2sdk::ConCommandContext::Console, {"mm_kill", "100%Player"});
		Check(HasChatAllPrint("100%Player"), "a '%' in a name is printed literally");
	}

	// mm_who: the flag name is data (it may contain a '%') and the server console has no tags.
	{
		g_adminFlags[10] = {"b"};
		g_flagNames["b"] = "100% flags";
		g_adminDataNotReady.clear();
	g_slotHandleOverrides.clear();
	g_entityHandleOverrides.clear();
	g_rejectedHandles.clear();
		ResetPrints();
		g_messageType = 0;
		DispatchCommand("mm_who", -1, s2sdk::ConCommandContext::Console, {"mm_who", "Root"});
		Check(HasServerPrint("100% flags"), "a '%' in a flag name is printed literally");
		Check(!HasServerPrint("{LIGHTRED}"), "the server console output is stripped of tags");
	}

	// The client console keeps the raw phrase (the Pisex PrintToConsole behaviour).
	{
		Grant(10, "@admin/kill");
		ResetPrints();
		DispatchCommand("mm_kill", 10, s2sdk::ConCommandContext::Console, {"mm_kill"});
		Check(HasConsolePrint("{LIGHTRED}"), "the client console gets the phrase as the original sent it");
	}

	FinishTest();
}

// --- I. unload and reload (PITFALLS A7) ------------------------------------------------------

void TestUnloadAndReload() {
	PrepareTest();
	MakeHuman(10, "Admin", 76561198000000010ULL);
	MakeHuman(11, "Target", 76561198000000011ULL);
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "start");
	LatchMap();
	Grant(10, "@admin/kill");

	// Open a menu and leave a selection pending, then unload.
	SelectAdminItem(10, "kill");
	Update();
	const FakeMenu* menu = OpenMenuOf(10);
	Check(menu != nullptr, "a menu is open before the unload");
	const s2sdk::MenuId openId = menu != nullptr ? menu->id : 0;
	if (menu != nullptr) {
		SelectByDisplay(10, "Target");
	}
	const s2sdk::ConCommandCallback savedCallback = FindAddedCommand("mm_kill") != nullptr
													? FindAddedCommand("mm_kill")->callback
													: nullptr;

	StopPlugin();
	Check(openId != 0 && g_destroyedMenuIds.end() !=
							 std::find(g_destroyedMenuIds.begin(), g_destroyedMenuIds.end(), openId),
		  "the open menu is destroyed on unload");
	Check(g_destroyMenuCalls > 0, "the menu manager destroy was called");
	for (const CommandDef& def : g_commands) {
		Check(RemovedCommand(def.szName), std::string(def.szName) + " is removed on unload");
	}
	Check(g_mapStartUnregisters == 1, "the map start listener is unregistered");
	Check(g_mapEndUnregisters == 1, "the map end listener is unregistered");
	Check(g_coreLoadedUnregisters == 1, "the admin core listener is unregistered");
	Check(g_pAdminApi == nullptr, "the admin API pointer is cleared");

	// A stale command callback cannot reach a client after the unload.
	ResetClientCounters();
	ResetPrints();
	if (savedCallback != nullptr) {
		Check(savedCallback(-1, s2sdk::ConCommandContext::Console, ToArgs({"mm_kill", "Target"})) ==
				  s2sdk::ResultType::Handled,
			  "the stale callback is still handled");
	}
	Check(g_clientCalls == 0, "a stale callback makes no client call after the unload");
	Check(g_forceSuicides.empty(), "a stale callback performs no action after the unload");

	// The update loop after the unload is inert.
	Update();
	Check(g_clientCalls == 0, "the update after the unload makes no client call");

	// A second unload is idempotent.
	StopPlugin();
	Check(g_mapStartUnregisters == 1, "a second unload does not unregister the map listener twice");

	// A reload starts cleanly and re-registers everything.
	MakeHuman(10, "Admin", 76561198000000010ULL);
	g_addedCommands.clear();
	g_removedCommands.clear();
	g_removedListeners.clear();
	g_mapStartRegisters = 0;
	g_mapEndRegisters = 0;
	g_coreLoadedRegisters = 0;
	Check(StartPlugin().GetCode() == plg::PluginCode::Ok, "reload start");
	Check(g_addedCommands.size() == 8, "the reload registers the commands again");
	Check(g_mapStartRegisters == 1 && g_mapEndRegisters == 1, "the reload registers the map listeners");
	Check(g_items.size() == 8, "the reload registers the admin items");
	LatchMap();
	MakeHuman(11, "Target", 76561198000000011ULL);
	Grant(10, "@admin/kill");
	DispatchCommand("mm_kill", -1, s2sdk::ConCommandContext::Console, {"mm_kill", "Target"});
	Check(g_forceSuicides.size() == 1, "the reloaded plugin works");

	// Unload without a map: a displayed menu is cancelled with Disconnect before the destroy.
	{
		Check(SelectAdminItem(10, "kill"), "kill item before the no-map unload");
		Update();
		Check(OpenMenuOf(10) != nullptr, "a menu is open before the no-map unload");
		g_mapReady = false; // the map ended but the plugin was not updated yet
		ResetClientCounters();
		g_cancelledMenus.clear();
		StopPlugin();
		Check(g_clientCalls == 0, "unloading without a map makes no client call");
		Check(!g_cancelledMenus.empty() &&
				  g_cancelledMenus.back().second == s2sdk::MenuCancelReason::Disconnect,
			  "the displayed menu is ended with Disconnect before it is destroyed");
		Check(OpenMenuOf(10) == nullptr, "the menu is gone after the no-map unload");
	}

	FinishTest();
}

} // namespace

int main() {
	InstallFakes();
	ResetWorld();

	TestStartCommandsAndContexts();
	TestPermissions();
	TestSignedHandles();
	TestPlayerList();
	TestSlotReuse();
	TestMapGate();
	TestMenuLifecycle();
	TestMenuTypeResolution();
	TestAdminItemRetirement();
	TestAdminItemRetirementFailSoft();
	TestMenuCleanupFailSafe();
	TestPairedBindingRegistration();
	TestMissingBindings();
	TestOutputAndPhrases();
	TestUnloadAndReload();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
