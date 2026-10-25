#ifdef __EMSCRIPTEN__

#include "common_types.h"
#include "generated_syms.h"

#include <stdint.h>

extern PlayerKeyState playerKeys[1];
extern unsigned char bg_itemlist[];
extern const char str_002157b8[];
extern const char str_002b51bc[], str_002b51d0[];
extern const char str_002b576c[], str_002b5778[], str_002b5788[], str_002b5794[];

/*
 * literals.S "Missing global variables" — web build does not link that .S.
 * bg_numItems must be real BSS/data: BG_FillInAmmoItems does *(int*)imp_bg_numItems.
 * If it resolves to the emscripten "missing function" stub, that read OOBs.
 */
int bg_numItems = 0;
float faceAxis[6][3];
int iSlotPreferenceOrder[16];
int refEntIsInWorldSpace = 0;
float sign[4];

qkey_t *keys = 0;
/*
 * Do NOT define __mh_execute_header as data here. Native/Win32 link with
 * -Wl,--defsym,__mh_execute_header=0x1000 so COD2_MH_EXECUTE_HEADER == 0x1000
 * (ADS button bit, DPVS LargeLocal size, dvar flags, etc.). A real data symbol
 * makes &__mh_execute_header a huge linear-memory address → world DPVS OOB and
 * empty map with skewed viewmodel.
 */
char name[64] __attribute__((aligned(4))) = {0};

void *TheStringPackage = 0;

/* s_codeSamplers / s_codeConsts / defaults live in r_material_code_tables.c.
 * Do not stub them here: --allow-multiple-definition would prefer these empty
 * tables and break technique binds (sampler.feedback, constant.materialColor). */

byte *_dvar_shellshock_fadein = 0;
byte *_dvar_shellshock_fadeout = 0;
byte *_dvar_shellshock_screenblend = 0;
byte *_dvar_shellshock_screentype = 0;
byte *_dvar_shellshock_screenenabled = 0;
byte *_dvar_shellshock_soundfadein = 0;
byte *_dvar_shellshock_soundfadeout = 0;
byte *_dvar_shellshock_loopfadein = 0;
byte *_dvar_shellshock_loopfadeout = 0;
byte *_dvar_shellshock_looptype = 0;
byte *_dvar_shellshock_sounddrylevellooptype = 0;
byte *_dvar_shellshock_soundwetlevellooptype = 0;
byte *_dvar_shellshock_soundloopsilent = 0;
byte *_dvar_shellshock_viewkickfadein = 0;
byte *_dvar_shellshock_viewkickperiod = 0;
byte *_dvar_shellshock_viewkickradius = 0;
byte *_dvar_shellshock_viewkickpitch = 0;
byte *_dvar_shellshock_viewkickyaw = 0;
byte *_dvar_shellshock_soundroomtype = 0;
byte *_dvar_shellshock_sounddrylevel = 0;
byte *_dvar_shellshock_soundwetlevel = 0;
byte *_dvar_shellshock_soundmodenddelay = 0;
byte *_dvar_shellshock_soundendduration = 0;
byte *_dvar_shellshock_soundfade = 0;
byte *_dvar_shellshock_mouseenable = 0;
byte *_dvar_shellshock_mousefadein = 0;
byte *_dvar_shellshock_mouseturnrate = 0;
byte *_dvar_shellshock_mousereducemax = 0;
byte *_dvar_shellshock_mousesensitivity = 0;

void *cg_dvar1;
void *cg_dvar2;
void *cg_dvar_debug;
void *cg_dvar_footsteps;
void *cg_dvar_shellshock_max;
void *cg_dvar_shellshock_min;
void *cg_entities_glob;
void *cg_glob;
void *_cg_p;
void *cg_ptr;
void *cgs_glob;
void *cgs_ptr;
void *clc_ptr;
void *clc_ptr_195ee8c;
void *cl_ptr;
void *cl_showPackets;
void *compPointerConfig;
void *dvar_autoPickup;
void *dvar_compassClampDist;
void *dvar_compassClampSaturation;
void *dvar_compassFade;
void *dvar_compassMaxRange;
void *dvar_compassMinRange;
void *dvar_compassMinSaturation;
void *dvar_compassPingFadeTime;
void *dvar_compassSize;
void *dvar_developer;
void *dvar_errorDecay;
void *dvar_nopredict;
void *dvar_synchronous;
void *g_time_ptr;
void *net_profile_dvar;
void *sm_dvar_ptr;
void *sm_mtl_init;
void *sm_mtl_vtable;
void *sv_dedicated_dvar;

void *cg_entities_ptr;
void *cls_ptr;
void *com_dvarflags_ptr;
void *dx_device_ptr;
void *dx_fence;
void *fx_developer_check_ptr;
void *fx_helper_ptr;
void *fx_scheduler_ptr;
void *g_backEndData;
void *g_bobMax_ptr;
void *g_debug_damage_ptr;
void *g_drawSurf;
void *gfx_buf;
void *gfxScene;
void *g_password_ptr;
void *g_renderState;
void *g_scr_data_ptr;
void *g_time;
void *g_viewInfo;
void *g_viewParms;
void *nextmap_dvar;
void *r_contentmask;
void *r_defaultColor;
void *r_dvar_ef30;
void *r_dvar_ef34;
void *r_dvar_ef38;
void *r_dvar_ef3c;
void *r_dvar_ef44;
void *r_dvar_ef48;
void *r_dvar_ef4c;
void *r_dvar_sunEnable;
void *r_frontEndDataOut;
void *r_glob;
void *r_lightGridContrast;
void *r_lightGridEnableTweaks;
void *r_lightGridSpread;
void *r_lightGridSunDir;
void *r_lightGridUseTweakedValues;
void *r_sundvar_table;
void *r_sunFlareState;
void *r_surftype_table_ptr;
void *r_videoConfig;
void *r_vtable_ptr;
void *r_world_ptr;
void *scrCompGlob_ptr;
void *sv_allowAnonymous_dvar;
void *sv_allowDownload_dvar;
void *sv_allowedClan1_dvar;
void *sv_allowedClan2_dvar;
void *sv_cheats_dvar;
void *sv_debugRate_dvar;
void *sv_debugReliableCmds_dvar;
void *sv_disableClientConsole_dvar;
void *sv_expectedHunkUsage_dvar;
void *sv_floodProtect_dvar;
void *sv_fps_dvar;
void *sv_gametype_dvar;
void *sv_hostname_dvar;
void *sv_iwdNames_dvar;
void *sv_iwds_dvar;
void *sv_kickBanTime_dvar;
void *sv_mapname_dvar;
void *sv_mapRotationCurrent_dvar;
void *sv_mapRotation_dvar;
void *sv_maxclients_dvar;
void *sv_maxPing_dvar;
void *sv_maxRate_dvar;
void *sv_minPing_dvar;
void *sv_minPingRate_dvar;
void *sv_packet_info_dvar;
void *sv_padPackets_dvar;
void *sv_privateClients_dvar;
void *sv_pure_dvar;
void *sv_reconnectlimit_dvar;
void *sv_referencedIwdNames_dvar;
void *sv_referencedIwds_dvar;
void *sv_running_dvar;
void *sv_serverid_dvar;
void *sv_showAverageBPS_dvar;
void *sv_showCommands_dvar;
void *sv_timeout_dvar;
void *sv_voice_dvar;
void *sv_voiceQuality_dvar;
void *sv_zombietime_dvar;
void *trace_mins;

/* web_gen/data.c still embeds Mac absolute 0x002157b8; rewrite to empty string. */
static void Web_FixBgItemlistRelocs(void)
{
    uint32_t *words = (uint32_t *)bg_itemlist;
    uint32_t empty = (uint32_t)(uintptr_t)str_002157b8;
    unsigned i;

    for (i = 0; i < (5824 / 4); i++) {
        if (words[i] == 0x002157b8u)
            words[i] = empty;
    }
}

__attribute__((constructor))
static void Web_InitWasmAliases(void)
{
    keys = playerKeys[0].keys;
    Web_FixBgItemlistRelocs();
    /* Restore data-pointer relocations from build/win_gen/data.win.S. Leaving
     * these zero crashes on the first grenade HUD or visible player model. */
    {
        extern unsigned char controller_names[44], offhandStrings[32], hintStrings[32], s_barrelTags[32];
        uintptr_t *controllers = (uintptr_t *)controller_names;
        uintptr_t *barrels = (uintptr_t *)s_barrelTags;
        const char **offhand = (const char **)offhandStrings;
        const char **hints = (const char **)hintStrings;
        for (int i = 0; i < 6; ++i)
            controllers[i] = (uintptr_t)imp_scr_const + 168 + i * 2;
        for (int i = 0; i < 4; ++i)
            barrels[i] = (uintptr_t)imp_scr_const + 140 + i * 2;
        offhand[0] = str_002157b8;
        offhand[1] = str_002b51bc;
        offhand[2] = str_002b51d0;
        hints[0] = str_002157b8;
        hints[1] = str_002b576c;
        hints[2] = str_002b5778;
        hints[3] = str_002b5788;
        hints[4] = str_002b5794;
    }

    /*
     * build/web_gen/data.c cannot express `.long <symbol>` relocations from
     * data.S and emits zeros instead. Restore the critical ones here — the
     * NULL theFxHelper made FX_WarpTime write the game time to address 0
     * ("corrupted its heap memory area (address zero)"), and the NULL FX
     * list/cluster pointers OOB'd during effect updates.
     */
    {
        extern unsigned char theFxHelper[4];
        extern unsigned char theFxHelpers[252];
        extern unsigned char effectListNonBolt[4];
        extern unsigned char effectListArrayNonBolt[7296];
        extern unsigned char effectListBolt[4];
        extern unsigned char effectListArrayBolt[7296];
        extern unsigned char effectClusters[16];
        extern unsigned char effectClusterArray[28800];
        extern unsigned char cmd_text[32];
        extern unsigned char cmd_texts[12];

        *(void **)&theFxHelper[0] = theFxHelpers;
        *(void **)&effectListNonBolt[0] = effectListArrayNonBolt;
        *(void **)&effectListBolt[0] = effectListArrayBolt;
        *(void **)&effectClusters[0] = effectClusterArray;
        *(void **)&cmd_text[0] = cmd_texts;
    }

    cg_dvar1 = imp_cg_nopredict;
    cg_dvar2 = imp_cg_synchronousClients;
    cg_dvar_debug = imp_cg_debugEvents;
    cg_dvar_footsteps = imp_cg_footsteps;
    cg_dvar_shellshock_max = imp_bg_fallDamageMaxHeight;
    cg_dvar_shellshock_min = imp_bg_fallDamageMinHeight;
    cg_entities_glob = imp_cg_entities;
    cg_glob = imp_cg;
    _cg_p = imp_cg;
    cg_ptr = imp_cg;
    cgs_glob = imp_cgs;
    cgs_ptr = imp_cgs;
    clc_ptr = imp_clc;
    clc_ptr_195ee8c = imp_clc;
    cl_ptr = imp_cl;
    cl_showPackets = imp_cl_shownuments;
    compPointerConfig = imp_cg_hudCompassSpringyPointers;
    dvar_autoPickup = imp_cg_predictItems;
    dvar_compassClampDist = imp_cg_hudObjectiveMaxRange;
    dvar_compassClampSaturation = imp_cg_hudObjectiveMinAlpha;
    dvar_compassFade = imp_hud_fade_compass;
    dvar_compassMaxRange = imp_cg_hudCompassMaxRange;
    dvar_compassMinRange = imp_cg_hudCompassMinRange;
    dvar_compassMinSaturation = imp_cg_hudCompassMinRadius;
    dvar_compassPingFadeTime = imp_cg_hudCompassSoundPingFadeTime;
    dvar_compassSize = imp_cg_hudCompassSize;
    dvar_developer = imp_cg_showmiss;
    dvar_errorDecay = imp_cg_errorDecay;
    dvar_nopredict = imp_cg_nopredict;
    dvar_synchronous = imp_cg_synchronousClients;
    g_time_ptr = imp_bgs;
    net_profile_dvar = imp_net_profile;
    sm_dvar_ptr = imp_com_statmon;
    sm_mtl_init = imp_cls;
    sm_mtl_vtable = imp_re;
    sv_dedicated_dvar = imp_com_dedicated;
    _dvar_shellshock_fadein = (byte *)imp_cg_shock_screenBlendFadeTime;
    _dvar_shellshock_fadeout = (byte *)imp_cg_shock_screenBlendTime;
    _dvar_shellshock_screenblend = (byte *)imp_cg_shock_viewKickPeriod;
    _dvar_shellshock_screentype = (byte *)imp_cg_shock_viewKickRadius;
    _dvar_shellshock_screenenabled = (byte *)imp_cg_shock_sound;
    _dvar_shellshock_soundfadein = (byte *)imp_cg_shock_soundFadeInTime;
    _dvar_shellshock_soundfadeout = (byte *)imp_cg_shock_soundFadeOutTime;
    _dvar_shellshock_loopfadein = (byte *)imp_cg_shock_soundLoopFadeTime;
    _dvar_shellshock_loopfadeout = (byte *)imp_cg_shock_soundLoopEndDelay;
    _dvar_shellshock_looptype = (byte *)imp_cg_shock_soundRoomType;
    _dvar_shellshock_sounddrylevellooptype = (byte *)imp_cg_shock_soundDryLevel;
    _dvar_shellshock_soundwetlevellooptype = (byte *)imp_cg_shock_soundWetLevel;
    _dvar_shellshock_soundloopsilent = (byte *)imp_cg_shock_soundModEndDelay;
    _dvar_shellshock_viewkickfadein = (byte *)imp_cg_shock_volume_auto;
    _dvar_shellshock_viewkickperiod = (byte *)imp_cg_shock_volume_auto2d;
    _dvar_shellshock_viewkickradius = (byte *)imp_cg_shock_volume_menu;
    _dvar_shellshock_viewkickpitch = (byte *)imp_cg_shock_volume_weapon;
    _dvar_shellshock_viewkickyaw = (byte *)imp_cg_shock_volume_voice;
    _dvar_shellshock_soundroomtype = (byte *)imp_cg_shock_volume_item;
    _dvar_shellshock_sounddrylevel = (byte *)imp_cg_shock_volume_body;
    _dvar_shellshock_soundwetlevel = (byte *)imp_cg_shock_volume_local;
    _dvar_shellshock_soundmodenddelay = (byte *)imp_cg_shock_volume_music;
    _dvar_shellshock_soundendduration = (byte *)imp_cg_shock_volume_announcer;
    _dvar_shellshock_soundfade = (byte *)imp_cg_shock_volume_shellshock;
    _dvar_shellshock_mouseenable = (byte *)imp_cg_shock_mouse;
    _dvar_shellshock_mousefadein = (byte *)imp_cg_shock_mouse_fadeTime;
    _dvar_shellshock_mouseturnrate = (byte *)imp_cg_shock_mouse_maxpitchspeed;
    _dvar_shellshock_mousereducemax = (byte *)imp_cg_shock_mouse_maxyawspeed;
    _dvar_shellshock_mousesensitivity = (byte *)imp_cg_shock_mouse_sensitivityscale;
    cg_entities_ptr = imp_cg_entities;
    cls_ptr = imp_cls;
    com_dvarflags_ptr = imp_dvar_modifiedFlags;
    dx_device_ptr = imp_dx;
    dx_fence = imp_alwaysfails;
    fx_developer_check_ptr = imp_g_rendererExists;
    fx_helper_ptr = imp_theFxHelper;
    fx_scheduler_ptr = imp_theFxScheduler;
    g_backEndData = imp_dxState;
    g_bobMax_ptr = imp_bg_bobMax;
    g_debug_damage_ptr = imp_g_debugDamage;
    g_drawSurf = imp_tess;
    gfx_buf = imp_tess;
    gfxScene = imp_scene;
    g_password_ptr = imp_g_password;
    g_renderState = imp_r_fog;
    g_scr_data_ptr = imp_scr_const;
    g_time = imp_level_bgs;
    g_viewInfo = imp_r_zfar;
    g_viewParms = imp_backEndData;
    nextmap_dvar = imp_nextmap;
    r_contentmask = imp_vec3_origin;
    r_defaultColor = imp_colorWhite;
    r_dvar_ef30 = imp_r_lightTweakSunColor;
    r_dvar_ef34 = imp_r_lightTweakDiffuseFraction;
    r_dvar_ef38 = imp_r_lightTweakAmbient;
    r_dvar_ef3c = imp_r_lightTweakAmbientColor;
    r_dvar_ef44 = imp_r_lightTweakSunDiffuseColor;
    r_dvar_ef48 = imp_r_lightTweakSunLight;
    r_dvar_ef4c = imp_r_lightTweakSunDirection;
    r_dvar_sunEnable = imp_r_drawSun;
    r_frontEndDataOut = imp_backEnd;
    r_glob = imp_rgp;
    r_lightGridContrast = imp_r_vc_makelog;
    r_lightGridEnableTweaks = imp_r_showLightGrid;
    r_lightGridSpread = imp_r_vc_showlog;
    r_lightGridSunDir = imp_vec3_colorintensity;
    r_lightGridUseTweakedValues = imp_r_showMissingLightGrid;
    r_sundvar_table = imp_s_sundvars;
    r_sunFlareState = imp_sunFlareArray;
    r_surftype_table_ptr = imp_infoParms;
    r_videoConfig = imp_vidConfig;
    r_vtable_ptr = imp_ri;
    r_world_ptr = imp_vec3_origin;
    scrCompGlob_ptr = imp_scrCompileGlob;
    sv_allowAnonymous_dvar = imp_sv_allowAnonymous;
    sv_allowDownload_dvar = imp_sv_allowDownload;
    sv_allowedClan1_dvar = imp_sv_allowedClan1;
    sv_allowedClan2_dvar = imp_sv_allowedClan2;
    sv_cheats_dvar = imp_sv_cheats;
    sv_debugRate_dvar = imp_sv_debugRate;
    sv_debugReliableCmds_dvar = imp_sv_debugReliableCmds;
    sv_disableClientConsole_dvar = imp_sv_disableClientConsole;
    sv_expectedHunkUsage_dvar = imp_com_expectedHunkUsage;
    sv_floodProtect_dvar = imp_sv_floodProtect;
    sv_fps_dvar = imp_sv_fps;
    sv_gametype_dvar = imp_sv_gametype;
    sv_hostname_dvar = imp_sv_hostname;
    sv_iwdNames_dvar = imp_sv_iwdNames;
    sv_iwds_dvar = imp_sv_iwds;
    sv_kickBanTime_dvar = imp_sv_kickBanTime;
    sv_mapname_dvar = imp_sv_mapname;
    sv_mapRotationCurrent_dvar = imp_sv_mapRotationCurrent;
    sv_mapRotation_dvar = imp_sv_mapRotation;
    sv_maxclients_dvar = imp_sv_maxclients;
    sv_maxPing_dvar = imp_sv_maxPing;
    sv_maxRate_dvar = imp_sv_maxRate;
    sv_minPing_dvar = imp_sv_minPing;
    sv_minPingRate_dvar = imp_sv_maxRate;
    sv_packet_info_dvar = imp_sv_packet_info;
    sv_padPackets_dvar = imp_sv_padPackets;
    sv_privateClients_dvar = imp_sv_privateClients;
    sv_pure_dvar = imp_sv_pure;
    sv_reconnectlimit_dvar = imp_sv_reconnectlimit;
    sv_referencedIwdNames_dvar = imp_sv_referencedIwdNames;
    sv_referencedIwds_dvar = imp_sv_referencedIwds;
    sv_running_dvar = imp_com_sv_running;
    sv_serverid_dvar = imp_sv_serverid;
    sv_showAverageBPS_dvar = imp_sv_showAverageBPS;
    sv_showCommands_dvar = imp_sv_showCommands;
    sv_timeout_dvar = imp_sv_timeout;
    sv_voice_dvar = imp_sv_voice;
    sv_voiceQuality_dvar = imp_sv_voiceQuality;
    sv_zombietime_dvar = imp_sv_zombietime;
    trace_mins = imp_vec3_origin;
}

#endif
