#include "stdint.h"
#include "sys/stat.h"
#include <GarrysMod/Lua/Interface.h>
#include <GarrysMod/FactoryLoader.hpp>
#include <GarrysMod/Symbol.hpp>
#include <scanning/symbolfinder.hpp>
#include <detouring/hook.hpp>
#include <Platform.hpp>
#include <steam/isteamclient.h>
#include <steam/isteamuser.h>
#include <unordered_map>

#define MAX_PLAYERS		129

static const char steamclient_name[] = "SteamClient012";

class IClient;

namespace global {

    SourceSDK::FactoryLoader engine_loader( "engine" );

    typedef void (* tBroadcastVoiceData ) ( IClient * , int , char * , int64  ) ;
    //static Symbol sym_BroadcastVoiceData = Symbol::FromName("_Z21SV_BroadcastVoiceDataP7IClientiPcx");
    static Symbol sym_BroadcastVoiceData = Symbol::FromSignature("\x55\x48\x8D\x05****\x48\x89\xE5\x41\x57\x41\x56\x41\x89\xF6\x41\x55\x49\x89\xFD\x41\x54\x49\x89\xD4\x53\x48\x89\xCB\x48\x81\xEC****\x48\x8B\x3D****\x48\x39\xC7\x74\x25");

    typedef int (* tGetPlayerSlot)( IClient * );
    tGetPlayerSlot fGetPlayerSlot = NULL;
    static Symbol sym_GetPlayerSlot = Symbol::FromName("_ZNK11CBaseClient13GetPlayerSlotEv");

    static Detouring::Hook bvd_hook;

    static int intercepts = 0;

    HSteamPipe g_hPipe;
    HSteamUser g_hUser;
    ISteamUser* g_user;

    std::unordered_map<uint64_t, FILE*> PlayerVoiceFileMap {};
    

    LUA_FUNCTION_STATIC(GetIntercepts)
    {
        LUA->PushNumber(intercepts);
        return 1;
    }

    inline uint64_t GetEntitySteamid64( GarrysMod::Lua::ILuaBase *LUA, int i )
    {
        LUA->Push( i );
        LUA->GetField( -1, "GetVoiceID" );
        LUA->Push( -2 );
        LUA->Call( 1, 1 );

        return static_cast<uint64_t>( LUA->GetNumber( -1 ) );
    }

    LUA_FUNCTION_STATIC( End )
    {
        LUA->CheckType(1, GarrysMod::Lua::Type::Entity);
        int steamid = GetEntitySteamid64(LUA, 1);

        //
        // fclose and remove
        if (PlayerVoiceFileMap.find(steamid) != PlayerVoiceFileMap.end()) {
            fclose(PlayerVoiceFileMap[steamid]);
            PlayerVoiceFileMap.erase( steamid );
        }


        return 0;
    }


    void hook_BroadcastVoiceData(IClient * pClient, int nBytes, char * data, int64 xuid)
    {
        bvd_hook.GetTrampoline<tBroadcastVoiceData>()(pClient, nBytes, data, xuid);
        intercepts++;

        #if defined ARCHITECTURE_X86
		uint64_t playerslot = *(uint64_t*)((char*)pClient + 181);
        #else
        uint64_t playerslot = *(uint64_t*)((char*)pClient + 189);
        #endif
        
        // a function that returns 0


        if (pClient && nBytes && data) {
            if (PlayerVoiceFileMap.find(playerslot) == PlayerVoiceFileMap.end()) {
                char fname[64];
                sprintf(fname, "garrysmod/data/voicehook/%ld.dat", playerslot);
                PlayerVoiceFileMap[ playerslot ] = fopen(fname, "ab");
            }
            FILE* voice_file = PlayerVoiceFileMap[ playerslot ];



            int nVoiceBytes = nBytes;
            char *pVoiceData = data;

            if (g_user) {
                static char decompressed[20000];

                uint32 numUncompressedBytes = 0; 
                int res = g_user->DecompressVoice(pVoiceData, nVoiceBytes,
                    decompressed, sizeof(decompressed), &numUncompressedBytes, 44100 );

                if ( res == k_EVoiceResultOK && numUncompressedBytes > 0 )
                {
                    fwrite(decompressed, 1, numUncompressedBytes, voice_file);
                }
            }
        }
    }

    void start(GarrysMod::Lua::ILuaBase* LUA) {
        if (!engine_loader.IsValid( )) return;

        struct stat st;
        if (stat("garrysmod/data/voicehook", &st) == -1) {
            mkdir("garrysmod/data/voicehook", 0777);
        }

		SymbolFinder symfinder;
        tBroadcastVoiceData original_BroadcastVoiceData = reinterpret_cast<tBroadcastVoiceData>(symfinder.Resolve(
            engine_loader.GetModule( ),
            sym_BroadcastVoiceData.name.c_str( ),
            sym_BroadcastVoiceData.length
        ));
        if (!original_BroadcastVoiceData) {
            LUA->ThrowError( "unable to find BroadcastVoiceData" );
            return;
        }
		if( !bvd_hook.Create( reinterpret_cast<void *>( original_BroadcastVoiceData ),
			reinterpret_cast<void *>( &hook_BroadcastVoiceData ) ) )
			LUA->ThrowError( "unable to create detour for BroadcastVoiceData" );
        bvd_hook.Enable();

        /*fGetPlayerSlot = reinterpret_cast<tGetPlayerSlot>(symfinder.Resolve(
            engine_loader.GetModule( ),
            sym_GetPlayerSlot.name.c_str( ),
            sym_GetPlayerSlot.length
        ));
        if (!fGetPlayerSlot) {
            LUA->ThrowError( "unable to find GetPlayerSlot" );
        }*/

        SourceSDK::FactoryLoader* mod = new SourceSDK::FactoryLoader("steamclient");
        if (!mod->IsValid()) {
            LUA->ThrowError( "failed to find steamclient" );
            return;
        }

        ISteamClient* g_pSteamClient = mod->GetInterface<ISteamClient>(steamclient_name);
        if(!g_pSteamClient)
        {
            LUA->ThrowError( "failed to acquire steamclient pointer" );
            return;
        }

        g_hUser = g_pSteamClient->CreateLocalUser(&g_hPipe, k_EAccountTypeIndividual);
        g_user = g_pSteamClient->GetISteamUser(g_hUser, g_hPipe, "SteamUser020");

        LUA->CreateTable( );

		LUA->PushCFunction( GetIntercepts );
        LUA->SetField( -2, "InterceptCount" );

		LUA->PushCFunction( End );
        LUA->SetField( -2, "End" );

        LUA->SetField( GarrysMod::Lua::INDEX_GLOBAL, "voicehook" );
    }

    void stop(GarrysMod::Lua::ILuaBase* LUA) {
        bvd_hook.Destroy();
    }
}
GMOD_MODULE_OPEN()
{
    global::start(LUA);
	return 0;
}

GMOD_MODULE_CLOSE( )
{
    for(auto & file : global::PlayerVoiceFileMap) {
        fclose( file.second );
    }

    global::stop(LUA);

	return 0;
}
