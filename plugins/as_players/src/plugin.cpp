// as_players — Plugify port of the Pisex Metamod:Source module "Players" (as_players).
//
// Admin player actions: mm_kill, mm_kick, mm_slap, mm_rename, mm_changeteam, mm_switchteam,
// mm_noclip, mm_who, plus the "players" admin-menu category with the same eight items (who,
// noclip, switchteam, changeteam, rename, slap, kick, kill), player picker, team picker and
// damage picker menus. Commands, menu items, phrases, permission strings and the action
// parameters sent to the admin core (SendAction) are kept from the original.
//
// Porting notes (differences to the Metamod original):
//   * CCSPlayerController/CCSPlayerPawn access goes through s2sdk + pisexcompat; entity handles
//     are the 1-based indices of s2sdk instead of CEntityInstance pointers.
//   * FindUser() matches the server-side player name (the original read the client's "name"
//     convar - the same string in practice) and compares the *full* SteamID64 (the original
//     truncated it to 32 bits), and accepts a player slot number like the original did.
//   * KickClient() replaces the original's engine->DisconnectClient kick.
//   * Slap damage reduces the health directly (s2sdk's TakeClientDamage needs valid inflictor and
//     attacker slots: a null slot makes it a no-op), and the original's SetStateChanged call on
//     the health field is kept.
//   * The move-type toggle uses s2sdk's Get/SetClientMoveType (MoveType::Noclip/Walk) instead of
//     the pawn's own move-type accessors.

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>

#include <pisex/pisex.hpp>

#include <admin_api.hpp>

#include <s2sdk/s2sdk.hpp>

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

// The Pisex spelling: g_pUtils / g_pMenus / g_pPlayers and the free helpers, unqualified.
using namespace pisex;

namespace {

constexpr const char* kLogTag = "Players";
constexpr int kSlots = 64;

// ---------------------------------------------------------------------------------------------
// Interface handles (the original's g_p*Api globals).
// ---------------------------------------------------------------------------------------------
admin_system::AdminApi* g_pAdminApi = nullptr;

// The identity of the item the admin picked last (the original's strdup'd szLastItem[] array).
std::array<std::string, kSlots> g_szLastItem;

// Tagged error log. The format is checked by the compiler.
void LogError(const char* format, ...) __attribute__((format(printf, 1, 2)));
void LogError(const char* format, ...) {
	std::array<char, 2048> buffer{};
	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer.data(), buffer.size(), format, args);
	va_end(args);
	ErrorLog("[%s] %s", kLogTag, buffer.data());
}

const char* T(const char* key) {
	return g_pAdminApi ? g_pAdminApi->GetTranslation(key) : key;
}

bool ValidSlot(int iSlot) {
	return iSlot >= 0 && iSlot < kSlots;
}

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------
bool ContainsOnlyDigits(const std::string& str) {
	return !str.empty() && str.find_first_not_of("0123456789") == std::string::npos;
}

// strcasestr() of the original.
bool NameContains(const char* szName, const std::string& szIdentity) {
	if (!szName || szIdentity.empty()) {
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
// slot number (all three were tried with the raw argument).
int FindUser(const char* szIdentity) {
	if (!szIdentity || !*szIdentity) {
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
	for (int i = 0; i < kSlots; ++i) {
		if (GetControllerEntity(i) == 0) {
			continue; // no controller entity for the slot
		}
		const uint64_t iSteamID = GetSteamID64(i);
		if (iSteamID == 0) {
			continue; // the original required a non-zero steam id
		}
		if (NameContains(GetPlayerName(i), sIdentity)) {
			return i;
		}
		if (bDigits && (static_cast<long long>(iSteamID) == iValue || iValue == i)) {
			return i;
		}
	}
	return -1;
}

// The arguments of the command line the engine's CCommand::ArgS() returns (command name stripped).
std::string ArgsAfterCommand(const char* szContent) {
	const std::string sContent(szContent ? szContent : "");
	const std::size_t iPos = sContent.find(' ');
	return iPos == std::string::npos ? std::string{} : sContent.substr(iPos + 1);
}

// The original's "usage message to a console or a player" block.
void PrintUsage(int iSlot, const char* szPhrase, const char* szCommand) {
	const std::string sMessage = FormatString(T(szPhrase), szCommand ? szCommand : "");
	// A negative slot is the server console (pisexcompat handles it), like META_CONPRINTF did.
	PrintToConsole(iSlot, "%s\n", sMessage.c_str());
}

// ---------------------------------------------------------------------------------------------
// Actions (port of the original's free functions)
// ---------------------------------------------------------------------------------------------
void KickPlayer(int iSlot, int iTarget) {
	if (!__s2sdk_KickClient) {
		return;
	}
	try {
		s2sdk::KickClient(iTarget, s2sdk::NetworkDisconnectionReason::Kicked, "");
	} catch (...) {
		LogError("could not kick slot %d", iTarget);
	}
	if (g_pAdminApi->GetMessageType() == 2) {
		PrintToChatAll(T("AdminKickMessage"), iSlot == -1 ? "Console" : g_pPlayers->GetPlayerName(iSlot),
					   g_pPlayers->GetPlayerName(iTarget));
	} else if (g_pAdminApi->GetMessageType() == 1) {
		PrintToChatAll(T("KickMessage"), g_pPlayers->GetPlayerName(iTarget));
	}
	g_pAdminApi->SendAction(iSlot, "kick", std::to_string(iTarget).c_str());
}

void KillPlayer(int iSlot, int iTarget) {
	if (GetPawnEntity(iTarget) == 0) {
		return; // the original required the controller's pawn
	}
	CommitSuicide(iTarget, false, true);
	if (g_pAdminApi->GetMessageType() == 2) {
		PrintToChatAll(T("AdminKillMessage"), iSlot == -1 ? "Console" : g_pPlayers->GetPlayerName(iSlot),
					   g_pPlayers->GetPlayerName(iTarget));
	} else if (g_pAdminApi->GetMessageType() == 1) {
		PrintToChatAll(T("KillMessage"), g_pPlayers->GetPlayerName(iTarget));
	}
	g_pAdminApi->SendAction(iSlot, "kill", std::to_string(iTarget).c_str());
}

void ChangePlayerName(int iSlot, int iTarget, const char* szName) {
	const std::string sOldName = GetPlayerName(iTarget);
	if (g_pAdminApi->GetMessageType() == 2) {
		PrintToChatAll(T("AdminRenameMessage"), iSlot == -1 ? "Console" : g_pPlayers->GetPlayerName(iSlot),
					   sOldName.c_str(), szName ? szName : "");
	} else if (g_pAdminApi->GetMessageType() == 1) {
		PrintToChatAll(T("RenameMessage"), sOldName.c_str(), szName ? szName : "");
	}
	SetPlayerName(iTarget, szName ? szName : "");
	const std::string sParam = std::to_string(iTarget) + " " + (szName ? szName : "");
	g_pAdminApi->SendAction(iSlot, "rename", sParam.c_str());
}

void NoclipPlayer(int iSlot, int iTarget) {
	if (GetPawnEntity(iTarget) == 0 || !__s2sdk_GetClientMoveType || !__s2sdk_SetClientMoveType) {
		return;
	}
	bool bNoclipNow = false;
	try {
		bNoclipNow = s2sdk::GetClientMoveType(iTarget) == s2sdk::MoveType::Noclip;
		SetMoveType(iTarget, static_cast<int>(bNoclipNow ? s2sdk::MoveType::Walk : s2sdk::MoveType::Noclip));
		// The original re-read the pawn's move type for the message after setting it.
		bNoclipNow = s2sdk::GetClientMoveType(iTarget) == s2sdk::MoveType::Noclip;
	} catch (...) {
		LogError("could not toggle noclip for slot %d", iTarget);
		return;
	}
	g_pAdminApi->SendAction(iSlot, "noclip", std::to_string(iTarget).c_str());
	if (g_pAdminApi->GetMessageType() == 2) {
		PrintToChatAll(T(bNoclipNow ? "AdminNoclipMessageEnable" : "AdminNoclipMessageDisable"),
					   iSlot == -1 ? "Console" : g_pPlayers->GetPlayerName(iSlot),
					   g_pPlayers->GetPlayerName(iTarget));
	} else if (g_pAdminApi->GetMessageType() == 1) {
		PrintToChatAll(T(bNoclipNow ? "NoclipMessageEnable" : "NoclipMessageDisable"),
					   g_pPlayers->GetPlayerName(iTarget));
	}
}

void WhoPlayer(int iSlot, int iTarget, bool bConsole) {
	const std::vector<std::string> vFlags = g_pAdminApi->GetAdminFlags(iTarget);
	if (!vFlags.empty()) {
		if (bConsole) {
			// The original's console print took only the phrase, not the name.
			PrintToConsole(iSlot, "%s\n", T("Item_Who_Flags"));
		} else {
			PrintToChat(iSlot, T("Item_Who_Flags"), g_pPlayers->GetPlayerName(iTarget));
		}
		for (const std::string& flag : vFlags) {
			if (bConsole) {
				PrintToConsole(iSlot, "%s\n", g_pAdminApi->GetFlagName(flag.c_str()));
			} else {
				// The flag name is translated data, not a format string: it may contain a '%'.
				PrintToChat(iSlot, "%s", g_pAdminApi->GetFlagName(flag.c_str()));
			}
		}
	} else {
		if (bConsole) {
			PrintToConsole(iSlot, "%s\n", T("Item_Who_NoFlags"));
		} else {
			// The phrase is translated data, not a format string: it may contain a '%'.
			PrintToChat(iSlot, "%s", T("Item_Who_NoFlags"));
		}
	}
	g_pAdminApi->SendAction(iSlot, "who", std::to_string(iTarget).c_str());
}

void SlapPlayer(int iSlot, int iTarget, int iDamage) {
	const int iPawn = GetPawnEntity(iTarget);
	if (iPawn == 0 || !__s2sdk_GetClientAbsVelocity || !__s2sdk_SetClientAbsVelocity) {
		return; // the original required the controller's pawn
	}
	try {
		plg::vec3 velocity = s2sdk::GetClientAbsVelocity(iTarget);
		velocity.x += static_cast<float>((rand() % 180) + 50) * (((rand() % 2) == 1) ? -1.0f : 1.0f);
		velocity.y += static_cast<float>((rand() % 180) + 50) * (((rand() % 2) == 1) ? -1.0f : 1.0f);
		velocity.z += static_cast<float>(rand() % 200 + 100);
		s2sdk::SetClientAbsVelocity(iTarget, velocity);
	} catch (...) {
		LogError("could not slap slot %d", iTarget);
		return;
	}
	if (iDamage > 0) {
		const int iHealth = GetPlayerHealth(iTarget);
		if (iDamage >= iHealth) {
			// A lethal slap kills the pawn: the original committed suicide instead.
			CommitSuicide(iTarget, false, true);
		} else if (__s2sdk_SetClientHealth) {
			try {
				s2sdk::SetClientHealth(iTarget, iHealth - iDamage);
			} catch (...) {
			}
			// The original marked the health field as changed after TakeDamage.
			SetStateChanged(iPawn, "CBaseEntity", "m_iHealth");
		}
	}
	const std::string sParam = std::to_string(iTarget) + " " + std::to_string(iDamage);
	g_pAdminApi->SendAction(iSlot, "slap", sParam.c_str());
	if (g_pAdminApi->GetMessageType() == 2) {
		PrintToChatAll(T("AdminSlapMessage"), iSlot == -1 ? "Console" : g_pPlayers->GetPlayerName(iSlot),
					   g_pPlayers->GetPlayerName(iTarget), std::to_string(iDamage).c_str());
	} else if (g_pAdminApi->GetMessageType() == 1) {
		PrintToChatAll(T("SlapMessage"), g_pPlayers->GetPlayerName(iTarget), std::to_string(iDamage).c_str());
	}
}

void ChangePlayerTeam(int iSlot, int iTarget, int iTeam, bool bSwitch) {
	if (iTeam < 0 || iTeam > 3) {
		return;
	}
	if (bSwitch) {
		SwitchTeam(iTarget, iTeam);
	} else {
		ChangeTeam(iTarget, iTeam);
	}
	const std::string sParam = std::to_string(iTarget) + " " + std::to_string(iTeam);
	g_pAdminApi->SendAction(iSlot, "changeteam", sParam.c_str());
	constexpr const char* teams[] = {"none", "spectators", "terrorists", "counter-terrorists"};
	if (g_pAdminApi->GetMessageType() == 2) {
		PrintToChatAll(T(bSwitch ? "AdminSwitchTeamMessage" : "AdminChangeTeamMessage"),
					   iSlot == -1 ? "Console" : g_pPlayers->GetPlayerName(iSlot),
					   g_pPlayers->GetPlayerName(iTarget), T(teams[iTeam]));
	} else if (g_pAdminApi->GetMessageType() == 1) {
		PrintToChatAll(T(bSwitch ? "SwitchTeamMessage" : "ChangeTeamMessage"),
					   g_pPlayers->GetPlayerName(iTarget), T(teams[iTeam]));
	}
}

// ---------------------------------------------------------------------------------------------
// Menus
// ---------------------------------------------------------------------------------------------
void OnItemSelect(int iSlot, const char* szCategory, const char* szIdentity, const char* szItem);

void SelectTeamMenu(int iSlot, int iTarget, bool bSwitch) {
	Menu hMenu;
	g_pMenus->SetTitleMenu(hMenu, T("SelectTeam"));
	g_pMenus->AddItemMenu(hMenu, "1", "SPEC");
	g_pMenus->AddItemMenu(hMenu, "2", "T");
	g_pMenus->AddItemMenu(hMenu, "3", "CT");
	g_pMenus->SetBackMenu(hMenu, true);
	g_pMenus->SetExitMenu(hMenu, true);
	g_pMenus->SetCallback(hMenu, [iTarget, bSwitch](const char* szBack, const char*, int iItem, int iSlot) {
		if (iItem < 7) {
			ChangePlayerTeam(iSlot, iTarget, std::atoi(szBack ? szBack : "0"), bSwitch);
		} else if (iItem == 7) {
			OnItemSelect(iSlot, "players", "changeteam", "changeteam");
		}
	});
	g_pMenus->DisplayPlayerMenu(hMenu, iSlot, true, true);
}

void SlapMenu(int iSlot, int iTarget) {
	Menu hMenu;
	g_pMenus->SetTitleMenu(hMenu, T("Item_Slap_Damage"));
	g_pMenus->AddItemMenu(hMenu, "0", "0");
	g_pMenus->AddItemMenu(hMenu, "1", "1");
	g_pMenus->AddItemMenu(hMenu, "5", "5");
	g_pMenus->AddItemMenu(hMenu, "10", "10");
	g_pMenus->AddItemMenu(hMenu, "20", "20");
	g_pMenus->AddItemMenu(hMenu, "50", "50");
	g_pMenus->AddItemMenu(hMenu, "100", "100");
	g_pMenus->SetBackMenu(hMenu, true);
	g_pMenus->SetExitMenu(hMenu, true);
	g_pMenus->SetCallback(hMenu, [iTarget](const char* szBack, const char*, int iItem, int iSlot) {
		if (iItem < 7) {
			SlapPlayer(iSlot, iTarget, std::atoi(szBack ? szBack : "0"));
		} else if (iItem == 7) {
			OnItemSelect(iSlot, "players", "slap", "slap");
		}
	});
	g_pMenus->DisplayPlayerMenu(hMenu, iSlot, true, true);
}

std::string GetRandomName() {
	std::string sRandomName;
	for (int i = 0; i < 10; ++i) {
		const int randValue = rand() % 52;
		const char c = randValue < 26 ? static_cast<char>('a' + randValue) : static_cast<char>('A' + (randValue - 26));
		sRandomName += c;
	}
	return sRandomName;
}

void OnItemSelect(int iSlot, const char* /*szCategory*/, const char* szIdentity, const char* szItem) {
	if (!ValidSlot(iSlot)) {
		return;
	}
	g_szLastItem[iSlot] = szIdentity ? szIdentity : "";
	const std::string sIdentity = g_szLastItem[iSlot];
	const bool bIsAlive = sIdentity == "kill" || sIdentity == "slap" || sIdentity == "noclip";
	Menu hMenu;
	g_pMenus->SetTitleMenu(hMenu, T("SelectPlayer"));
	for (int i = 0; i < kSlots; ++i) {
		if (GetControllerEntity(i) == 0 || GetPawnEntity(i) == 0) {
			continue; // the original required the controller and its pawn
		}
		if (bIsAlive && !IsAlive(i)) {
			continue;
		}
		if (sIdentity == "noclip") {
			bool bNoclip = false;
			if (__s2sdk_GetClientMoveType) {
				try {
					bNoclip = s2sdk::GetClientMoveType(i) == s2sdk::MoveType::Noclip;
				} catch (...) {
				}
			}
			const std::string sText = std::string(GetPlayerName(i)) + " [" + (bNoclip ? "ON" : "OFF") + "]";
			g_pMenus->AddItemMenu(hMenu, std::to_string(i).c_str(), sText.c_str());
		} else {
			g_pMenus->AddItemMenu(hMenu, std::to_string(i).c_str(), GetPlayerName(i));
		}
	}
	g_pMenus->SetBackMenu(hMenu, true);
	g_pMenus->SetExitMenu(hMenu, true);
	g_pMenus->SetCallback(hMenu, [szItem](const char* szBack, const char*, int iItem, int iSlot) {
		if (!ValidSlot(iSlot)) {
			return;
		}
		if (iItem < 7) {
			const int iTarget = std::atoi(szBack ? szBack : "-1");
			if (GetControllerEntity(iTarget) == 0 || GetPawnEntity(iTarget) == 0) {
				return;
			}
			const std::string sLastItem = g_szLastItem[iSlot];
			if (sLastItem == "kill") {
				KillPlayer(iSlot, iTarget);
				OnItemSelect(iSlot, "players", "kill", "reopen");
			} else if (sLastItem == "kick") {
				KickPlayer(iSlot, iTarget);
				OnItemSelect(iSlot, "players", "kick", "reopen");
			} else if (sLastItem == "slap") {
				SlapMenu(iSlot, iTarget);
			} else if (sLastItem == "rename") {
				ChangePlayerName(iSlot, iTarget, GetRandomName().c_str());
			} else if (sLastItem == "changeteam") {
				SelectTeamMenu(iSlot, iTarget, false);
			} else if (sLastItem == "switchteam") {
				SelectTeamMenu(iSlot, iTarget, true);
			} else if (sLastItem == "noclip") {
				NoclipPlayer(iSlot, iTarget);
				OnItemSelect(iSlot, "players", "noclip", "reopen");
			} else if (sLastItem == "who") {
				WhoPlayer(iSlot, iTarget, false);
			}
		} else if (iItem == 7) {
			g_pAdminApi->ShowAdminLastCategoryMenu(iSlot);
		}
	});
	g_pMenus->DisplayPlayerMenu(hMenu, iSlot, true, (szItem && std::strcmp(szItem, "reopen") == 0) ? false : true);
}

// ---------------------------------------------------------------------------------------------
// Console commands (the original's CON_COMMAND_F bodies)
// ---------------------------------------------------------------------------------------------
bool OnKillCommand(int iSlot, const char* szContent) {
	const bool bConsole = iSlot == -1;
	if (!g_pAdminApi->HasPermission(iSlot, "@admin/kill") && !bConsole) {
		return true;
	}
	CommandArgs args;
	args.Tokenize(szContent);
	if (args.ArgC() < 2) {
		PrintUsage(iSlot, "UsageDefault", args.Arg(0));
		return true;
	}
	const int iTarget = FindUser(args.Arg(1));
	if (iTarget == -1) {
		return true;
	}
	KillPlayer(iSlot, iTarget);
	return true;
}

bool OnKickCommand(int iSlot, const char* szContent) {
	const bool bConsole = iSlot == -1;
	if (!g_pAdminApi->HasPermission(iSlot, "@admin/kick") && !bConsole) {
		return true;
	}
	CommandArgs args;
	args.Tokenize(szContent);
	if (args.ArgC() < 2) {
		PrintUsage(iSlot, "UsageDefault", args.Arg(0));
		return true;
	}
	const int iTarget = FindUser(args.Arg(1));
	if (iTarget == -1) {
		return true;
	}
	KickPlayer(iSlot, iTarget);
	return true;
}

bool OnSlapCommand(int iSlot, const char* szContent) {
	const bool bConsole = iSlot == -1;
	if (!g_pAdminApi->HasPermission(iSlot, "@admin/slap") && !bConsole) {
		return true;
	}
	CommandArgs args;
	args.Tokenize(szContent);
	if (args.ArgC() < 3) {
		PrintUsage(iSlot, "UsageSlap", args.Arg(0));
		return true;
	}
	const int iTarget = FindUser(args.Arg(1));
	if (iTarget == -1) {
		return true;
	}
	const std::string sDamage = args.Arg(2) ? args.Arg(2) : "";
	if (!sDamage.empty() &&
		(sDamage.length() > 3 || sDamage.find_first_not_of("0123456789") != std::string::npos)) {
		PrintUsage(iSlot, "UsageSlap", args.Arg(0));
		return true;
	}
	SlapPlayer(iSlot, iTarget, std::atoi(sDamage.c_str()));
	return true;
}

bool OnRenameCommand(int iSlot, const char* szContent) {
	const bool bConsole = iSlot == -1;
	if (!g_pAdminApi->HasPermission(iSlot, "@admin/rename") && !bConsole) {
		return true;
	}
	CommandArgs args;
	args.Tokenize(szContent);
	if (args.ArgC() < 3) {
		PrintUsage(iSlot, "UsageRename", args.Arg(0));
		return true;
	}
	const int iTarget = FindUser(args.Arg(1));
	if (iTarget == -1) {
		return true;
	}
	// The original took the raw argument string, dropped its first character and then the first
	// argument's length.
	std::string sName = ArgsAfterCommand(szContent);
	if (!sName.empty()) {
		sName.erase(0, 1);
	}
	sName.erase(0, std::strlen(args.Arg(1) ? args.Arg(1) : ""));
	const std::string sOldName = GetPlayerName(iTarget);
	SetPlayerName(iTarget, sName.c_str());
	const std::string sParam = std::to_string(iTarget) + " " + sName;
	g_pAdminApi->SendAction(iSlot, "rename", sParam.c_str());
	if (g_pAdminApi->GetMessageType() == 2) {
		PrintToChatAll(T("AdminRenameMessage"), iSlot == -1 ? "Console" : g_pPlayers->GetPlayerName(iSlot),
					   sOldName.c_str(), sName.c_str());
	} else if (g_pAdminApi->GetMessageType() == 1) {
		PrintToChatAll(T("RenameMessage"), sOldName.c_str(), sName.c_str());
	}
	return true;
}

bool ChangeTeamCommand(int iSlot, const char* szContent, bool bSwitch, const char* szPermission) {
	const bool bConsole = iSlot == -1;
	if (!g_pAdminApi->HasPermission(iSlot, szPermission) && !bConsole) {
		return true;
	}
	CommandArgs args;
	args.Tokenize(szContent);
	if (args.ArgC() < 3) {
		PrintUsage(iSlot, "UsageSwitchTeam", args.Arg(0));
		return true;
	}
	const int iTarget = FindUser(args.Arg(1));
	if (iTarget == -1) {
		return true;
	}
	const std::string sTeam = args.Arg(2) ? args.Arg(2) : "";
	if (!sTeam.empty() && (sTeam.length() != 1 || !std::isdigit(static_cast<unsigned char>(sTeam[0])))) {
		PrintUsage(iSlot, "UsageSwitchTeam", args.Arg(0));
		return true;
	}
	ChangePlayerTeam(iSlot, iTarget, std::atoi(sTeam.c_str()), bSwitch);
	return true;
}

bool OnChangeTeamCommand(int iSlot, const char* szContent) {
	return ChangeTeamCommand(iSlot, szContent, false, "@admin/changeteam");
}

bool OnSwitchTeamCommand(int iSlot, const char* szContent) {
	return ChangeTeamCommand(iSlot, szContent, true, "@admin/switchteam");
}

bool OnNoclipCommand(int iSlot, const char* szContent) {
	const bool bConsole = iSlot == -1;
	if (!g_pAdminApi->HasPermission(iSlot, "@admin/noclip") && !bConsole) {
		return true;
	}
	CommandArgs args;
	args.Tokenize(szContent);
	if (args.ArgC() < 2) {
		PrintUsage(iSlot, "UsageDefault", args.Arg(0));
		return true;
	}
	const int iTarget = FindUser(args.Arg(1));
	if (iTarget == -1) {
		return true;
	}
	NoclipPlayer(iSlot, iTarget);
	return true;
}

bool OnWhoCommand(int iSlot, const char* szContent) {
	const bool bConsole = iSlot == -1;
	if (!g_pAdminApi->HasPermission(iSlot, "@admin/who") && !bConsole) {
		return true;
	}
	CommandArgs args;
	args.Tokenize(szContent);
	if (args.ArgC() < 2) {
		PrintUsage(iSlot, "UsageDefault", args.Arg(0));
		return true;
	}
	const int iTarget = FindUser(args.Arg(1));
	if (iTarget == -1) {
		return true;
	}
	WhoPlayer(iSlot, iTarget, true);
	return true;
}

// The original registered the menu items when the admin core signalled that it is loaded.
void OnAdminCoreLoaded() {
	g_pAdminApi->RegisterCategory("players", "Category_Players", nullptr);
	g_pAdminApi->RegisterItem("who", "Item_Who", "players", "@admin/who", nullptr, OnItemSelect);
	g_pAdminApi->RegisterItem("noclip", "Item_Noclip", "players", "@admin/noclip", nullptr, OnItemSelect);
	g_pAdminApi->RegisterItem("switchteam", "Item_SwitchTeam", "players", "@admin/switchteam", nullptr, OnItemSelect);
	g_pAdminApi->RegisterItem("changeteam", "Item_ChangeTeam", "players", "@admin/changeteam", nullptr, OnItemSelect);
	g_pAdminApi->RegisterItem("rename", "Item_Rename", "players", "@admin/rename", nullptr, OnItemSelect);
	g_pAdminApi->RegisterItem("slap", "Item_Slap", "players", "@admin/slap", nullptr, OnItemSelect);
	g_pAdminApi->RegisterItem("kick", "Item_Kick", "players", "@admin/kick", nullptr, OnItemSelect);
	g_pAdminApi->RegisterItem("kill", "Item_Kill", "players", "@admin/kill", nullptr, OnItemSelect);
}

// ---------------------------------------------------------------------------------------------
// Plugin glue
// ---------------------------------------------------------------------------------------------
class PlayersPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override {
		try {
			// Never Failed: a plugin that fails OnPluginStart stops every Plugify plugin.
			Init("admin_system"); // <configs>/admin_system (+ translations)
			g_pAdminApi = &admin_system::AdminApi::Get();

			g_pUtils->RegServerCommand({"mm_kill"}, OnKillCommand);
			g_pUtils->RegServerCommand({"mm_kick"}, OnKickCommand);
			g_pUtils->RegServerCommand({"mm_slap"}, OnSlapCommand);
			g_pUtils->RegServerCommand({"mm_rename"}, OnRenameCommand);
			g_pUtils->RegServerCommand({"mm_changeteam"}, OnChangeTeamCommand);
			g_pUtils->RegServerCommand({"mm_switchteam"}, OnSwitchTeamCommand);
			g_pUtils->RegServerCommand({"mm_noclip"}, OnNoclipCommand);
			g_pUtils->RegServerCommand({"mm_who"}, OnWhoCommand);

			g_pAdminApi->OnCoreLoaded(OnAdminCoreLoaded);
			if (g_pAdminApi->IsCoreLoaded()) {
				OnAdminCoreLoaded(); // late load: the event will not fire twice
			}
			return plg::PluginResult{};
		} catch (const std::exception& ex) {
			LogError("OnPluginStart failed: %s", ex.what());
			return plg::PluginResult{};
		} catch (...) {
			LogError("OnPluginStart failed with an unknown exception");
			return plg::PluginResult{};
		}
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds) override {
		// The plugify update callback must not let an exception through.
		try {
			Poll();
		} catch (const std::exception& ex) {
			LogError("update failed: %s", ex.what());
		} catch (...) {
			LogError("update failed with an unknown exception");
		}
		return plg::PluginResult{};
	}

	plg::PluginResult OnPluginEnd() override {
		try {
			Shutdown();
			if (g_pAdminApi) {
				g_pAdminApi->OnCoreLoadedUnregister();
			}
		} catch (...) {
		}
		return plg::PluginResult{};
	}
};

PlayersPlugin g_playersPlugin;

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_playersPlugin)
