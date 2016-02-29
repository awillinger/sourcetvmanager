/**
 * vim: set ts=4 :
 * =============================================================================
 * SourceMod SourceTV Manager Extension
 * Copyright (C) 2004-2016 AlliedModders LLC.  All rights reserved.
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, AlliedModders LLC gives you permission to link the
 * code of this program (as well as its derivative works) to "Half-Life 2," the
 * "Source Engine," the "SourcePawn JIT," and any Game MODs that run on software
 * by the Valve Corporation.  You must obey the GNU General Public License in
 * all respects for all other code used.  Additionally, AlliedModders LLC grants
 * this exception to all derivative works.  AlliedModders LLC defines further
 * exceptions, found in LICENSE.txt (as of this writing, version JULY-31-2007),
 * or <http://www.sourcemod.net/license.php>.
 *
 * Version: $Id$
 */

#include "extension.h"
#include "forwards.h"

IHLTVDirector *hltvdirector = nullptr;
IHLTVServer *hltvserver = nullptr;
IDemoRecorder *demorecorder = nullptr;
void *host_client = nullptr;
void *old_host_client = nullptr;
bool g_HostClientOverridden = false;

IGameEventManager2 *gameevents = nullptr;

IBinTools *bintools = nullptr;
ISDKTools *sdktools = nullptr;
IServer *iserver = nullptr;
IGameConfig *g_pGameConf = nullptr;

SH_DECL_HOOK1_void(IHLTVDirector, AddHLTVServer, SH_NOATTRIB, 0, IHLTVServer *);
SH_DECL_HOOK1_void(IHLTVDirector, RemoveHLTVServer, SH_NOATTRIB, 0, IHLTVServer *);

SH_DECL_HOOK1(IClient, ExecuteStringCommand, SH_NOATTRIB, 0, bool, const char *);

/**
 * @file extension.cpp
 * @brief Implement extension code here.
 */

SourceTVManager g_STVManager;		/**< Global singleton for extension's main interface */

SMEXT_LINK(&g_STVManager);

extern const sp_nativeinfo_t sourcetv_natives[];

bool SourceTVManager::SDK_OnLoad(char *error, size_t maxlength, bool late)
{
	sharesys->AddDependency(myself, "bintools.ext", true, true);
	sharesys->AddDependency(myself, "sdktools.ext", true, true);

	char conf_error[255];
	if (!gameconfs->LoadGameConfigFile("sourcetvmanager.games", &g_pGameConf, conf_error, sizeof(conf_error)))
	{
		if (error)
		{
			snprintf(error, maxlength, "Could not read sourcetvmanager.games: %s", conf_error);
		}
		return false;
	}

	// Get the host_client pointer
	// This is used to fix a null pointer crash when executing fake commands on bots.
	if (!g_pGameConf->GetAddress("host_client", &host_client) || !host_client)
	{
		smutils->LogError(myself, "Failed to find host_client pointer. Server might crash when executing commands on SourceTV bot.");
	}

	sharesys->AddNatives(myself, sourcetv_natives);
	sharesys->RegisterLibrary(myself, "sourcetvmanager");

	return true;
}

void SourceTVManager::SDK_OnAllLoaded()
{
	SH_ADD_HOOK(IHLTVDirector, AddHLTVServer, hltvdirector, SH_MEMBER(this, &SourceTVManager::OnAddHLTVServer_Post), true);
	SH_ADD_HOOK(IHLTVDirector, RemoveHLTVServer, hltvdirector, SH_MEMBER(this, &SourceTVManager::OnRemoveHLTVServer_Post), true);

	SM_GET_LATE_IFACE(BINTOOLS, bintools);
	SM_GET_LATE_IFACE(SDKTOOLS, sdktools);

	g_pSTVForwards.Init();

	iserver = sdktools->GetIServer();
	if (!iserver)
		smutils->LogError(myself, "Failed to get IServer interface from SDKTools. Some functions won't work.");

	if (hltvdirector->GetHLTVServerCount() > 0)
		SelectSourceTVServer(hltvdirector->GetHLTVServer(0));

	// Hook all the exisiting servers.
	for (int i = 0; i < hltvdirector->GetHLTVServerCount(); i++)
	{
		g_pSTVForwards.HookRecorder(GetDemoRecorderPtr(hltvdirector->GetHLTVServer(i)));
	}
}

bool SourceTVManager::SDK_OnMetamodLoad(ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	GET_V_IFACE_CURRENT(GetServerFactory, hltvdirector, IHLTVDirector, INTERFACEVERSION_HLTVDIRECTOR);
	GET_V_IFACE_CURRENT(GetEngineFactory, gameevents, IGameEventManager2, INTERFACEVERSION_GAMEEVENTSMANAGER2);

	return true;
}

void SourceTVManager::SDK_OnUnload()
{
	SH_REMOVE_HOOK(IHLTVDirector, AddHLTVServer, hltvdirector, SH_MEMBER(this, &SourceTVManager::OnAddHLTVServer_Post), true);
	SH_REMOVE_HOOK(IHLTVDirector, RemoveHLTVServer, hltvdirector, SH_MEMBER(this, &SourceTVManager::OnRemoveHLTVServer_Post), true);

	gameconfs->CloseGameConfigFile(g_pGameConf);

	// Unhook all the existing servers.
	for (int i = 0; i < hltvdirector->GetHLTVServerCount(); i++)
	{
		g_pSTVForwards.UnhookRecorder(GetDemoRecorderPtr(hltvdirector->GetHLTVServer(i)));
	}
	g_pSTVForwards.Shutdown();
}

bool SourceTVManager::QueryRunning(char *error, size_t maxlength)
{
	SM_CHECK_IFACE(BINTOOLS, bintools);
	SM_CHECK_IFACE(SDKTOOLS, sdktools);

	return true;
}

void SourceTVManager::SelectSourceTVServer(IHLTVServer *hltv)
{
	// Need to unhook the old server first?
	if (hltvserver != nullptr && iserver != nullptr)
	{
		IClient *pClient = iserver->GetClient(hltvserver->GetHLTVSlot());
		if (pClient)
		{
			SH_REMOVE_HOOK(IClient, ExecuteStringCommand, pClient, SH_MEMBER(this, &SourceTVManager::OnHLTVBotExecuteStringCommand), false);
			SH_REMOVE_HOOK(IClient, ExecuteStringCommand, pClient, SH_MEMBER(this, &SourceTVManager::OnHLTVBotExecuteStringCommand_Post), true);
		}
	}

	// Select the new server.
	hltvserver = hltv;

	UpdateDemoRecorderPointer();
	
	if (!hltvserver)
		return;

	if (!iserver)
		return;
	IClient *pClient = iserver->GetClient(hltvserver->GetHLTVSlot());
	if (!pClient)
		return;

	SH_ADD_HOOK(IClient, ExecuteStringCommand, pClient, SH_MEMBER(this, &SourceTVManager::OnHLTVBotExecuteStringCommand), false);
	SH_ADD_HOOK(IClient, ExecuteStringCommand, pClient, SH_MEMBER(this, &SourceTVManager::OnHLTVBotExecuteStringCommand_Post), true);
}

IDemoRecorder *SourceTVManager::GetDemoRecorderPtr(IHLTVServer *hltvserver)
{
	static int offset = -1;
	if (offset == -1 && !g_pGameConf->GetOffset("CHLTVServer::m_DemoRecorder", &offset))
	{
		smutils->LogError(myself, "Failed to get CHLTVServer::m_DemoRecorder offset.");
		return nullptr;
	}

	if (hltvserver)
		return (IDemoRecorder *)((intptr_t)hltvserver + offset);
	else
		return nullptr;
}

void SourceTVManager::UpdateDemoRecorderPointer()
{
	demorecorder = GetDemoRecorderPtr(hltvserver);
}

void SourceTVManager::OnAddHLTVServer_Post(IHLTVServer *hltv)
{
	g_pSTVForwards.HookRecorder(GetDemoRecorderPtr(hltv));

	// We already selected some SourceTV server. Keep it.
	if (hltvserver != nullptr)
		RETURN_META(MRES_IGNORED);
	
	// This is the first SourceTV server to be added. Hook it.
	SelectSourceTVServer(hltv);
}

void SourceTVManager::OnRemoveHLTVServer_Post(IHLTVServer *hltv)
{
	g_pSTVForwards.UnhookRecorder(GetDemoRecorderPtr(hltv));

	// We got this SourceTV server selected. Now it's gone :(
	if (hltvserver == hltv)
	{
		// Is there another one available? Try to keep us operable.
		if (hltvdirector->GetHLTVServerCount() > 0)
			SelectSourceTVServer(hltvdirector->GetHLTVServer(0));
		else
			SelectSourceTVServer(nullptr);
	}
}

bool SourceTVManager::OnHLTVBotExecuteStringCommand(const char *s)
{
	if (!hltvserver || !iserver || !host_client)
		RETURN_META_VALUE(MRES_IGNORED, 0);

	if (*(void **)host_client)
		RETURN_META_VALUE(MRES_IGNORED, 0);

	IClient *pClient = iserver->GetClient(hltvserver->GetHLTVSlot());
	if (!pClient)
		RETURN_META_VALUE(MRES_IGNORED, 0);

	// The IClient vtable is +4 from the CBaseClient vtable due to multiple inheritance.
	void *pGameClient = (void *)((intptr_t)pClient - 4);

	old_host_client = *(void **)host_client;
	*(void **)host_client = pGameClient;
	g_HostClientOverridden = true;

	RETURN_META_VALUE(MRES_IGNORED, 0);
}

bool SourceTVManager::OnHLTVBotExecuteStringCommand_Post(const char *s)
{
	if (!host_client || !g_HostClientOverridden)
		RETURN_META_VALUE(MRES_IGNORED, 0);

	*(void **)host_client = old_host_client;
	g_HostClientOverridden = false;
	RETURN_META_VALUE(MRES_IGNORED, 0);
}
