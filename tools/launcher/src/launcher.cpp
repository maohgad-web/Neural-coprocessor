// MGPU Bridge Launcher - 0.4.0 (R320: 2nd pass row, Diagnose benches the 2nd pass the game runs; R320a: COPY FILES replaces mgpu.ini, keeps Dcomp*, optional backup)
// 0.3.0, released with the bridge 0.3.0 (LAUNCHER_LEDGER)
//
// Drawn the way the add-on's idle screen is drawn (src/screen.cpp, R108): an
// 8% grey field, grey text with the second line at 60%, one slow sweep bar,
// and colour spent only when something is wrong. Until 0.3.0 the text used
// screen.cpp's 5x7 one-bit font; since 0.3.0 every language is drawn with a
// Windows font (Marcelo: one font for all), English in upper case as the 5x7
// font drew it.
//
// THE PAGE ANSWERS, FIRST AND LARGE, from the record the add-on writes
// (mgpu\last_launch.ini, R267/R270): which card rendered the game and at
// what frame rate, which card did the neural work and at what frame rate
// (the slower one is the bottleneck), and whether the neural output landed
// on the main display. Under that, the LAYOUT is read from this PC, not
// asked: every active display above the card its cable goes to (kernel
// mapping, D3DKMTOpenAdapterFromGdiDisplayName - the add-on's R265 proved it
// follows the cable), every GPU, the main display's cable lit. The verdict
// under the picture comes from the last launch; if the add-on recorded the
// display elsewhere, the red line says so and names what the person can
// change. The launcher cannot decide which card renders the game - that is
// Windows' and the game's - it can only report where the neural output
// landed, on which card, at what latency.
//
// WHAT IT READS AND WRITES
//   %LOCALAPPDATA%\MGPU Bridge\launcher.ini   the library and the names (ours)
//   <game>\mgpu.ini                           DcompOverlay / DcompMultiDisplay /
//                                             DcompFit / LaunchRecord and the
//                                             OPTIONS keys, line-anchored, at SAVE.
//                                             An old Display= / DisplayName= is
//                                             removed at SAVE (0.3.0 SCAN round:
//                                             the add-on decides, R269 refused it
//                                             on the game's card)
//   <game>\mgpu\last_launch.ini               read only (the add-on writes it)
//   <game>\nrbench_result.ini                 read only (Diagnose writes it)
//   <launcher dir>\payload\*                  the bridge files COPY FILES copies
//
// WHAT IT NEVER DOES: run a game, change ReShade, touch the driver.
//
// DLL SEARCH. A game folder with ReShade in it has a dxgi.dll beside the
// game; run from there, the launcher would load it and be hooked (seen
// 2026-10-07). System DLLs come from System32 only, DXGI by explicit path.
//
// DIAGNOSE runs nrcheck.exe --bench (tools/nrcheck, R268) in the game folder
// on the card the bridge chose on the last launch (else the only NVIDIA card
// that does not drive Windows' main display - the game lands on the main
// display's card): a synthetic DLSS-NR run at a resolution and pass count,
// no game. It proves that card runs the model and at what cost. Whether the
// output lands on the display is answered only by a game launch.

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <dxgi1_4.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace
{
    // ---------------- ids ----------------
    enum : int {
        ID_LIST = 100, ID_ADD, ID_REMOVE,
        ID_INSTALL, ID_OPENLOGS,
        ID_DISPLAY_CHANGE,   // SCAN DISPLAY (0.3.0 SCAN round; CHOOSE DISPLAY before)
        ID_DIAG_RES, ID_DIAG_PASSES,
        ID_MULTI_TOGGLE, ID_RECORD_TOGGLE, ID_FIT_TOGGLE,
        ID_TAB_STATUS, ID_TAB_ADV, ID_TAB_GUIDE, ID_GUIDE_RESHADE, ID_GUIDE_RHI,   // tabs STATUS, OPTIONS, INSTALL
        ID_SAVE, ID_RENAME,
        ID_LANG_0, ID_LANG_LAST = ID_LANG_0 + 5,   // one per language, LANG_EN..LANG_KO
        // the options tab (writes the same mgpu.ini keys the ReShade panel writes)
        ID_ADV_TUNING_OFF = 200, ID_ADV_TUNING_ON,
        ID_ADV_STYLE_A, ID_ADV_STYLE_B, ID_ADV_STYLE_C,
        ID_ADV_TONE_M, ID_ADV_TONE_P, ID_ADV_STRUCT_M, ID_ADV_STRUCT_P, ID_ADV_SKIN_M, ID_ADV_SKIN_P,
        ID_ADV_MASK_OFF, ID_ADV_MASK_ON,
        ID_ADV_INT_M, ID_ADV_INT_P,
        ID_ADV_PASSES_1, ID_ADV_PASSES_2,
        ID_ADV_FG_OFF, ID_ADV_FG_MAP, ID_ADV_FG_FIX, ID_ADV_FG_THIN, ID_ADV_KEEP_M, ID_ADV_KEEP_P,
        ID_ADV_SR_OFF, ID_ADV_SR_ON, ID_ADV_SR_NATIVE, ID_ADV_SR_EXP,
        ID_ADV_SRQ_Q, ID_ADV_SRQ_B, ID_ADV_SRQ_P,
        ID_ADV_PRESET_0, ID_ADV_PRESET_K, ID_ADV_PRESET_L, ID_ADV_PRESET_M,
        ID_ADV_CROP_OFF, ID_ADV_CROP_ON, ID_ADV_CROP_AUTO,
        ID_ADV_N16_OFF, ID_ADV_N16_ON, ID_ADV_N16P_M, ID_ADV_N16P_P,
        ID_ADV_P2_MED, ID_ADV_P2_LIGHT, ID_ADV_P2_FULL,  // R320: the 2nd pass (the panel's R319 choice)
        ID_ADV_NRS_OFF, ID_ADV_NRS_ON,                   // R321: pass 1 scale (NRS, R299) - the panel's switch
        // R356 (0.5.0): the panel's "Pass settings" - network and resolution per pass, FG on the DLSS 5 card,
        // and the restore button. Green = the FPS+ choice, red = the FPS- one (draw_button).
        ID_ADV_P1N_LIGHT, ID_ADV_P1N_FULL, ID_ADV_P1R_RES, ID_ADV_P1R_NATIVE,
        ID_ADV_P2N_LIGHT, ID_ADV_P2N_FULL, ID_ADV_P2R_RES, ID_ADV_P2R_NATIVE,
        ID_ADV_FG1_OFF, ID_ADV_FG1_ON, ID_ADV_BEST,
        ID_ADV_P1X2_LIGHT, ID_ADV_P1X2_FULL   // R361: pass 1's network while x2+ runs (NRPreset1x2)
    };
    const UINT_PTR TIMER_SWEEP = 1;

    // ---------------- the palette (screen.cpp's) ----------------
    const COLORREF C_BASE = RGB(8, 8, 9);        // 0.030 0.032 0.035
    const COLORREF C_FG   = RGB(158, 163, 173);  // 0.62 0.64 0.68
    const COLORREF C_DIM  = RGB(95, 98, 104);    // fg * 0.6
    const COLORREF C_RULE = RGB(30, 31, 34);
    const COLORREF C_SEL  = RGB(18, 19, 21);
    const COLORREF C_RED  = RGB(199, 87, 82);    // 0.78 0.34 0.32 - error only
    const COLORREF C_LIT  = RGB(214, 218, 226);  // the chosen display and its cable
    const COLORREF C_GRN  = RGB(104, 176, 112);  // R329: OPTIMIZATION's state (QUALITY MAX / ON / FPS MAX)
    const COLORREF C_GRN_D = RGB(52, 88, 56);    // R356: an unselected FPS+ button
    const COLORREF C_RED_D = RGB(100, 44, 41);   // R356: an unselected FPS- button

    HINSTANCE g_hinst = nullptr;
    HWND g_main = nullptr, g_list = nullptr,
         g_btn_add = nullptr, g_btn_remove = nullptr,
         g_btn_install = nullptr, g_btn_logs = nullptr,
         g_btn_change = nullptr, g_btn_res = nullptr, g_btn_passes = nullptr,
         g_btn_multi = nullptr, g_btn_record = nullptr, g_btn_fit = nullptr,
         g_tab_status = nullptr, g_tab_adv = nullptr, g_tab_guide = nullptr,
         g_btn_g_reshade = nullptr, g_btn_g_rhi = nullptr,
         g_btn_save = nullptr, g_btn_rename = nullptr;
    int g_tab = 0;                         // 0 status, 1 options, 2 install (the guide)
    std::vector<HWND> g_adv_ctls;          // the options tab's buttons
    std::vector<int> g_active_ids;         // buttons drawn as "selected"
    HBRUSH g_br_base = nullptr;

    std::wstring g_cfg_dir, g_cfg_path, g_exe_dir;
    std::vector<std::wstring> g_library;
    std::vector<std::wstring> g_names;     // the name each library entry shows (empty = guessed from the path)
    // 0.3.0 SAVE: every change on the STATUS and OPTIONS pages goes to a working
    // copy of the selected game's mgpu.ini; SAVE writes it to the file.
    std::wstring g_work_game;              // whose mgpu.ini the working copy is
    std::string g_work_ini, g_disk_ini;    // the working copy, and the file as last read or saved
    std::wstring g_global_display, g_global_display_name;   // GDI name + friendly
    int  g_res_idx = 1;      // 1920x1080, 2560x1440, 3440x1440, 3840x2160 (res_follow_pc sets it)
    bool g_res_user = false; // the person picked the resolution with its button: it is kept
    const wchar_t *k_res[] = { L"1920x1080", L"2560x1440", L"3440x1440", L"3840x2160" };
    const wchar_t *k_res_label[] = { L"1080P", L"1440P", L"3440X1440", L"4K" };
    int  g_passes = 1;
    unsigned long long g_frame = 0;   // the sweep

    // ---------------- geometry ----------------
    // 0.3.0: 80 wider and 32 taller than 0.5.0, so every language fits and the
    // status rows have room for the two values that can take a second line
    const int WIN_W = 1120, WIN_H = 950;
    const int LX = 20, LW = 270;              // library
    const int PX = 310, PW = 790;             // page (ends 1100)
    const int Y_LL = 100, Y_LAYOUT = 290, Y_PIC = 312;
    const int Y_ROWS = 586, ROW_H = 28;
    const int Y_SWEEP = WIN_H - 16;
    // The status rows, one index per line. Every button is placed by its row
    // (0.5.0 put the force-fit, multi-display and launch-record buttons two
    // rows below their lines). INSTALL and LAUNCH RECORD can take a second
    // line: the row under each has no buttons.
    enum : int { R_INSTALL, R_INSTALL2, R_DISPLAY, R_DIAG, R_DIAGLINE, R_RESO, R_RESO_DETAIL, R_RESO_FIX,
                 R_FIT, R_MULTI, R_RECORD, R_RECORD2, R_N };
    int st_txt_y(int r) { return Y_ROWS + 12 + r * ROW_H; }
    int st_btn_y(int r) { return Y_ROWS + 8 + r * ROW_H; }
    int g_row_end[R_N] = {};   // where row r's value ends: its buttons' left edge - 10, else the page edge (relayout_status)
    int g_tabs_x = PX + PW;    // the tabs' left edge (relayout_tabs)
    int g_name_btn_x = PX + PW;   // RENAME / SAVE's left edge on the game-name row (relayout_status)

    // ---------------- languages (0.3.0) ----------------
    //
    // Every language is drawn with a Windows font (0.3.0; until then English
    // used the 5x7 font, which has no accents and no CJK): Segoe UI for
    // English, Portuguese and Spanish, Microsoft YaHei UI, Yu Gothic UI and
    // Malgun Gothic for Chinese, Japanese and Korean - all ship with Windows.
    // English is drawn in upper case, as the 5x7 font drew it.
    // Every string drawn passes through tr_draw: a whole-string match in k_tr
    // (case-insensitive) is replaced, anything else (game names, paths, card
    // names, numbers) is drawn as it is. Strings built from pieces translate
    // each piece with tr() where they are built. The flags under the library
    // pick the language; launcher.ini keeps the choice (Language=).
    enum : int { LANG_EN = 0, LANG_PT, LANG_ES, LANG_ZH, LANG_JA, LANG_KO, LANG_N };
    int g_lang = LANG_EN;
    const char *const k_lang_code[LANG_N] = { "en", "pt-BR", "es", "zh", "ja", "ko" };
    const wchar_t *const k_lang_name[LANG_N] = { L"English", L"Portugu\u00EAs (Brasil)", L"Espa\u00F1ol", L"\u4E2D\u6587", L"\u65E5\u672C\u8A9E", L"\uD55C\uAD6D\uC5B4" };
    struct tr_row { const wchar_t *en; const wchar_t *t[LANG_N - 1]; };
    const tr_row k_tr[] = {
        { L"AN UNKNOWN CARD", { L"UMA PLACA DESCONHECIDA", L"UNA TARJETA DESCONOCIDA", L"\u672A\u77E5\u663E\u5361", L"\u4E0D\u660E\u306A\u30AB\u30FC\u30C9", L"\uC54C \uC218 \uC5C6\uB294 \uADF8\uB798\uD53D \uCE74\uB4DC" } },
        { L"PRIMARY", { L"PRINCIPAL", L"PRINCIPAL", L"\u4E3B\u663E\u793A\u5668", L"\u30E1\u30A4\u30F3", L"\uAE30\uBCF8" } },
        { L"CARD NOT RESOLVED", { L"PLACA N\u00C3O IDENTIFICADA", L"TARJETA NO IDENTIFICADA", L"\u65E0\u6CD5\u8BC6\u522B\u663E\u5361", L"\u30AB\u30FC\u30C9\u3092\u7279\u5B9A\u3067\u304D\u307E\u305B\u3093", L"\uCE74\uB4DC\uB97C \uD655\uC778\uD560 \uC218 \uC5C6\uC74C" } },
        { L"THE ADD-ON", { L"O ADD-ON", L"EL ADD-ON", L"\u63D2\u4EF6", L"\u30A2\u30C9\u30AA\u30F3", L"\uC560\uB4DC\uC628" } },
        { L"NO LAUNCH RECORDED YET", { L"NENHUMA EXECU\u00C7\u00C3O REGISTRADA", L"A\u00DAN NO HAY EJECUCIONES REGISTRADAS", L"\u5C1A\u65E0\u542F\u52A8\u8BB0\u5F55", L"\u8D77\u52D5\u8A18\u9332\u306F\u307E\u3060\u3042\u308A\u307E\u305B\u3093", L"\uC544\uC9C1 \uC2E4\uD589 \uAE30\uB85D \uC5C6\uC74C" } },
        { L"RUN THE GAME ONCE WITH THE BRIDGE INSTALLED", { L"EXECUTE O JOGO UMA VEZ COM O BRIDGE INSTALADO", L"EJECUTA EL JUEGO UNA VEZ CON EL BRIDGE INSTALADO", L"\u5B89\u88C5 Bridge \u540E\u8FD0\u884C\u4E00\u6B21\u6E38\u620F", L"Bridge \u3092\u5165\u308C\u305F\u72B6\u614B\u3067\u30B2\u30FC\u30E0\u3092\u4E00\u5EA6\u8D77\u52D5\u3057\u3066\u304F\u3060\u3055\u3044", L"Bridge\uB97C \uC124\uCE58\uD55C \uC0C1\uD0DC\uB85C \uAC8C\uC784\uC744 \uD55C \uBC88 \uC2E4\uD589\uD558\uC138\uC694" } },
        { L"ADAPTER ", { L"ADAPTADOR ", L"ADAPTADOR ", L"\u9002\u914D\u5668 ", L"\u30A2\u30C0\u30D7\u30BF\u30FC ", L"\uC5B4\uB311\uD130 " } },
        { L"UNKNOWN", { L"DESCONHECIDO", L"DESCONOCIDO", L"\u672A\u77E5", L"\u4E0D\u660E", L"\uC54C \uC218 \uC5C6\uC74C" } },
        { L"NONE", { L"NENHUMA", L"NINGUNA", L"\u65E0", L"\u306A\u3057", L"\uC5C6\uC74C" } },
        { L"NONE - NO CARD WAS SELECTED", { L"NENHUMA - NENHUMA PLACA FOI SELECIONADA", L"NINGUNA - NO SE SELECCION\u00D3 NINGUNA TARJETA", L"\u65E0 - \u672A\u9009\u62E9\u663E\u5361", L"\u306A\u3057 - \u30AB\u30FC\u30C9\u304C\u9078\u629E\u3055\u308C\u3066\u3044\u307E\u305B\u3093", L"\uC5C6\uC74C - \uC120\uD0DD\uB41C \uCE74\uB4DC \uC5C6\uC74C" } },
        { L" - THE SAME CARD AS THE GAME", { L" - A MESMA PLACA DO JOGO", L" - LA MISMA TARJETA QUE EL JUEGO", L" - \u4E0E\u6E38\u620F\u662F\u540C\u4E00\u5F20\u663E\u5361", L" - \u30B2\u30FC\u30E0\u3068\u540C\u3058\u30AB\u30FC\u30C9", L" - \uAC8C\uC784\uACFC \uAC19\uC740 \uCE74\uB4DC" } },
        { L"NOT RECORDED", { L"N\u00C3O REGISTRADO", L"NO REGISTRADO", L"\u672A\u8BB0\u5F55", L"\u8A18\u9332\u306A\u3057", L"\uAE30\uB85D \uC5C6\uC74C" } },
        { L"ON THE DLSS 5 CARD", { L"NA PLACA DLSS 5", L"EN LA TARJETA DLSS 5", L"\u5728 DLSS 5 \u663E\u5361\u4E0A", L"DLSS 5 \u30AB\u30FC\u30C9\u4E0A", L"DLSS 5 \uCE74\uB4DC\uC5D0 \uC5F0\uACB0\uB428" } },
        { L"ON THE GAME CARD", { L"NA PLACA DO JOGO", L"EN LA TARJETA DEL JUEGO", L"\u5728\u6E38\u620F\u663E\u5361\u4E0A", L"\u30B2\u30FC\u30E0\u30AB\u30FC\u30C9\u4E0A", L"\uAC8C\uC784 \uCE74\uB4DC\uC5D0 \uC5F0\uACB0\uB428" } },
        { L"ON NEITHER CARD", { L"EM NENHUMA DAS PLACAS", L"EN NINGUNA DE LAS TARJETAS", L"\u4E0D\u5728\u4EFB\u4F55\u4E00\u5F20\u663E\u5361\u4E0A", L"\u3069\u3061\u3089\u306E\u30AB\u30FC\u30C9\u3067\u3082\u306A\u3044", L"\uC5B4\uB290 \uCE74\uB4DC\uC5D0\uB3C4 \uC5C6\uC74C" } },
        { L"ARMED, NO FRAMES RECORDED YET   ", { L"ARMADO, NENHUM QUADRO REGISTRADO   ", L"ARMADO, A\u00DAN SIN FOTOGRAMAS REGISTRADOS   ", L"\u5DF2\u542F\u52A8\uFF0C\u5C1A\u672A\u8BB0\u5F55\u5E27   ", L"\u8D77\u52D5\u6E08\u307F\u3001\u30D5\u30EC\u30FC\u30E0\u8A18\u9332\u306A\u3057   ", L"\uC2DC\uC791\uB428, \uAE30\uB85D\uB41C \uD504\uB808\uC784 \uC5C6\uC74C   " } },
        { L"MAIN DISPLAY: NOT FOUND", { L"MONITOR PRINCIPAL: N\u00C3O ENCONTRADO", L"MONITOR PRINCIPAL: NO ENCONTRADO", L"\u4E3B\u663E\u793A\u5668\uFF1A\u672A\u627E\u5230", L"\u30E1\u30A4\u30F3\u30C7\u30A3\u30B9\u30D7\u30EC\u30A4\uFF1A\u898B\u3064\u304B\u308A\u307E\u305B\u3093", L"\uAE30\uBCF8 \uBAA8\uB2C8\uD130: \uCC3E\uC744 \uC218 \uC5C6\uC74C" } },
        { L"MAIN DISPLAY: ", { L"MONITOR PRINCIPAL: ", L"MONITOR PRINCIPAL: ", L"\u4E3B\u663E\u793A\u5668\uFF1A", L"\u30E1\u30A4\u30F3\u30C7\u30A3\u30B9\u30D7\u30EC\u30A4\uFF1A", L"\uAE30\uBCF8 \uBAA8\uB2C8\uD130: " } },
        { L" - ITS CARD COULD NOT BE RESOLVED", { L" - SUA PLACA N\u00C3O FOI IDENTIFICADA", L" - NO SE PUDO IDENTIFICAR SU TARJETA", L" - \u65E0\u6CD5\u8BC6\u522B\u5176\u663E\u5361", L" - \u63A5\u7D9A\u5148\u306E\u30AB\u30FC\u30C9\u3092\u7279\u5B9A\u3067\u304D\u307E\u305B\u3093", L" - \uC5F0\uACB0\uB41C \uCE74\uB4DC\uB97C \uD655\uC778\uD560 \uC218 \uC5C6\uC74C" } },
        { L" IS ON THE CARD THAT RENDERED THE GAME", { L" EST\u00C1 NA PLACA QUE RENDERIZOU O JOGO", L" EST\u00C1 EN LA TARJETA QUE RENDERIZ\u00D3 EL JUEGO", L" \u8FDE\u63A5\u5728\u6E32\u67D3\u6E38\u620F\u7684\u663E\u5361\u4E0A", L" \u306F\u30B2\u30FC\u30E0\u3092\u63CF\u753B\u3057\u305F\u30AB\u30FC\u30C9\u306B\u63A5\u7D9A\u3055\u308C\u3066\u3044\u307E\u3059", L" - \uAC8C\uC784\uC744 \uB80C\uB354\uB9C1\uD55C \uCE74\uB4DC\uC5D0 \uC5F0\uACB0\uB428" } },
        { L" IS ON THE IGPU", { L" EST\u00C1 NA IGPU", L" EST\u00C1 EN LA IGPU", L" \u8FDE\u63A5\u5728\u6838\u663E\u4E0A", L" \u306F\u5185\u8535 GPU \u306B\u63A5\u7D9A", L" - \uB0B4\uC7A5 GPU\uC5D0 \uC5F0\uACB0\uB428" } },
        { L" IS ON ", { L" EST\u00C1 NA ", L" EST\u00C1 EN ", L" \u8FDE\u63A5\u5728 ", L" \u306E\u63A5\u7D9A\u5148: ", L" - \uC5F0\uACB0 \uC704\uCE58: " } },
        { L"LAUNCH: NONE YET - RUN THE GAME ONCE, THE ADD-ON RECORDS IT", { L"EXECU\u00C7\u00C3O: NENHUMA AINDA - EXECUTE O JOGO UMA VEZ, O ADD-ON REGISTRA", L"EJECUCI\u00D3N: NINGUNA A\u00DAN - EJECUTA EL JUEGO UNA VEZ, EL ADD-ON LO REGISTRA", L"\u542F\u52A8\uFF1A\u6682\u65E0 - \u8FD0\u884C\u4E00\u6B21\u6E38\u620F\uFF0C\u63D2\u4EF6\u4F1A\u8BB0\u5F55", L"\u8D77\u52D5\uFF1A\u307E\u3060\u3042\u308A\u307E\u305B\u3093 - \u30B2\u30FC\u30E0\u3092\u4E00\u5EA6\u8D77\u52D5\u3059\u308B\u3068\u8A18\u9332\u3055\u308C\u307E\u3059", L"\uC2E4\uD589: \uC544\uC9C1 \uC5C6\uC74C - \uAC8C\uC784\uC744 \uD55C \uBC88 \uC2E4\uD589\uD558\uBA74 \uC560\uB4DC\uC628\uC774 \uAE30\uB85D" } },
        { L"LAUNCH: DLSS 5 OUTPUT LANDED ON ", { L"EXECU\u00C7\u00C3O: SA\u00CDDA DLSS 5 EXIBIDA EM ", L"EJECUCI\u00D3N: SALIDA DLSS 5 MOSTRADA EN ", L"\u542F\u52A8\uFF1ADLSS 5 \u8F93\u51FA\u663E\u793A\u5728 ", L"\u8D77\u52D5\uFF1ADLSS 5 \u51FA\u529B\u306E\u8868\u793A\u5148 ", L"\uC2E4\uD589: DLSS 5 \uCD9C\uB825 \uD45C\uC2DC \uC704\uCE58 " } },
        { L" (DLSS 5 CARD)", { L" (PLACA DLSS 5)", L" (TARJETA DLSS 5)", L"\uFF08DLSS 5 \u663E\u5361\uFF09", L"\uFF08DLSS 5 \u30AB\u30FC\u30C9\uFF09", L" (DLSS 5 \uCE74\uB4DC)" } },
        { L"LAUNCH: DLSS 5 WAS NOT OFFLOADED TO ", { L"EXECU\u00C7\u00C3O: O DLSS 5 N\u00C3O FOI ENVIADO PARA ", L"EJECUCI\u00D3N: EL DLSS 5 NO SE ENVI\u00D3 A ", L"\u542F\u52A8\uFF1ADLSS 5 \u672A\u8F6C\u79FB\u5230 ", L"\u8D77\u52D5\uFF1ADLSS 5 \u304C\u79FB\u3055\u308C\u306A\u304B\u3063\u305F\u5148 ", L"\uC2E4\uD589: DLSS 5\uAC00 \uB118\uC5B4\uAC00\uC9C0 \uC54A\uC74C - " } },
        { L"CHANGE WHICH GPU DRIVES YOUR MAIN DISPLAY, THE DESKTOP MODE", { L"MUDE A GPU DO MONITOR PRINCIPAL, O MODO DA \u00C1REA DE TRABALHO", L"CAMBIA QU\u00C9 GPU MANEJA TU MONITOR PRINCIPAL, EL MODO DE ESCRITORIO", L"\u8BF7\u66F4\u6539\u9A71\u52A8\u4E3B\u663E\u793A\u5668\u7684 GPU\u3001\u684C\u9762\u6A21\u5F0F", L"\u30E1\u30A4\u30F3\u30C7\u30A3\u30B9\u30D7\u30EC\u30A4\u3092\u99C6\u52D5\u3059\u308B GPU\u3001\u30C7\u30B9\u30AF\u30C8\u30C3\u30D7\u306E\u30E2\u30FC\u30C9", L"\uC8FC \uBAA8\uB2C8\uD130\uB97C \uAD6C\uB3D9\uD558\uB294 GPU, \uB370\uC2A4\uD06C\uD1B1 \uBAA8\uB4DC" } },
        { L"OR THE CABLE, THEN RUN THE GAME AGAIN", { L"OU O CABO, E EXECUTE O JOGO DE NOVO", L"O EL CABLE, Y VUELVE A EJECUTAR EL JUEGO", L"\u6216\u7EBF\u7F06\uFF0C\u7136\u540E\u518D\u6B21\u8FD0\u884C\u6E38\u620F", L"\u307E\u305F\u306F\u30B1\u30FC\u30D6\u30EB\u3092\u5909\u3048\u3066\u3001\u30B2\u30FC\u30E0\u3092\u518D\u8D77\u52D5\u3057\u3066\u304F\u3060\u3055\u3044", L"\uB610\uB294 \uCF00\uC774\uBE14\uC744 \uBC14\uAFBC \uB4A4 \uAC8C\uC784\uC744 \uB2E4\uC2DC \uC2E4\uD589\uD558\uC138\uC694" } },
        { L"DIAGNOSE: ", { L"DIAGN\u00D3STICO: ", L"DIAGN\u00D3STICO: ", L"\u8BCA\u65AD\uFF1A", L"\u8A3A\u65AD\uFF1A", L"\uC9C4\uB2E8: " } },
        { L" (SYNTHETIC, NO GAME)", { L" (SINT\u00C9TICO, SEM JOGO)", L" (SINT\u00C9TICO, SIN JUEGO)", L"\uFF08\u5408\u6210\u6D4B\u8BD5\uFF0C\u65E0\u6E38\u620F\uFF09", L"\uFF08\u5408\u6210\u30C6\u30B9\u30C8\u3001\u30B2\u30FC\u30E0\u306A\u3057\uFF09", L" (\uD569\uC131 \uD14C\uC2A4\uD2B8, \uAC8C\uC784 \uC5C6\uC74C)" } },
        { L"RESHADE NOT FOUND - SEE THE INSTALL PAGE, STEP 1", { L"RESHADE N\u00C3O ENCONTRADO - VEJA A P\u00C1GINA INSTALAR, PASSO 1", L"RESHADE NO ENCONTRADO - MIRA LA P\u00C1GINA INSTALAR, PASO 1", L"\u672A\u627E\u5230 RESHADE - \u89C1\u201C\u5B89\u88C5\u201D\u9875\u7B2C 1 \u6B65", L"RESHADE \u304C\u898B\u3064\u304B\u308A\u307E\u305B\u3093 - \u300C\u30A4\u30F3\u30B9\u30C8\u30FC\u30EB\u300D\u30DA\u30FC\u30B8\u306E\u624B\u9806 1", L"RESHADE \uC5C6\uC74C - \uC124\uCE58 \uD398\uC774\uC9C0 1\uB2E8\uACC4 \uCC38\uACE0" } },
        { L"MISSING NVNGX_DLSSNR.DLL - SEE THE INSTALL PAGE, STEPS 2 AND 3", { L"FALTA NVNGX_DLSSNR.DLL - VEJA A P\u00C1GINA INSTALAR, PASSOS 2 E 3", L"FALTA NVNGX_DLSSNR.DLL - MIRA LA P\u00C1GINA INSTALAR, PASOS 2 Y 3", L"\u7F3A\u5C11 NVNGX_DLSSNR.DLL - \u89C1\u201C\u5B89\u88C5\u201D\u9875\u7B2C 2\u30013 \u6B65", L"NVNGX_DLSSNR.DLL \u304C\u3042\u308A\u307E\u305B\u3093 - \u300C\u30A4\u30F3\u30B9\u30C8\u30FC\u30EB\u300D\u30DA\u30FC\u30B8\u306E\u624B\u9806 2\u30013", L"NVNGX_DLSSNR.DLL \uC5C6\uC74C - \uC124\uCE58 \uD398\uC774\uC9C0 2, 3\uB2E8\uACC4 \uCC38\uACE0" } },
        { L"MISSING ", { L"FALTA: ", L"FALTA: ", L"\u7F3A\u5C11\uFF1A", L"\u4E0D\u8DB3\uFF1A", L"\uC5C6\uC74C: " } },
        { L" - PRESS COPY FILES", { L" - CLIQUE EM COPIAR ARQUIVOS", L" - PULSA COPIAR ARCHIVOS", L" - \u8BF7\u70B9\u51FB\u201C\u590D\u5236\u6587\u4EF6\u201D", L" - \u300C\u30D5\u30A1\u30A4\u30EB\u3092\u30B3\u30D4\u30FC\u300D\u3092\u62BC\u3057\u3066\u304F\u3060\u3055\u3044", L" - \uD30C\uC77C \uBCF5\uC0AC\uB97C \uB204\uB974\uC138\uC694" } },
        { L"OTHER ADD-ONS: ", { L"OUTROS ADD-ONS: ", L"OTROS ADD-ONS: ", L"\u5176\u4ED6\u63D2\u4EF6\uFF1A", L"\u4ED6\u306E\u30A2\u30C9\u30AA\u30F3\uFF1A", L"\uB2E4\uB978 \uC560\uB4DC\uC628: " } },
        { L" - SEE THE INSTALL PAGE, STEP 5", { L" - VEJA A P\u00C1GINA INSTALAR, PASSO 5", L" - MIRA LA P\u00C1GINA INSTALAR, PASO 5", L" - \u89C1\u201C\u5B89\u88C5\u201D\u9875\u7B2C 5 \u6B65", L" - \u300C\u30A4\u30F3\u30B9\u30C8\u30FC\u30EB\u300D\u30DA\u30FC\u30B8\u306E\u624B\u9806 5", L" - \uC124\uCE58 \uD398\uC774\uC9C0 5\uB2E8\uACC4 \uCC38\uACE0" } },
        { L"ON - FPS PER CARD WRITTEN WHILE THE GAME RUNS", { L"LIGADO - FPS POR PLACA GRAVADO DURANTE O JOGO", L"ACTIVADO - FPS POR TARJETA MIENTRAS SE JUEGA", L"\u5F00\u542F - \u6E38\u620F\u8FD0\u884C\u65F6\u8BB0\u5F55\u6BCF\u5F20\u663E\u5361\u7684 FPS", L"\u30AA\u30F3 - \u30B2\u30FC\u30E0\u4E2D\u306B\u30AB\u30FC\u30C9\u3054\u3068\u306E FPS \u3092\u8A18\u9332", L"\uCF1C\uC9D0 - \uAC8C\uC784 \uC911 \uCE74\uB4DC\uBCC4 FPS \uAE30\uB85D" } },
        { L"OFF - TURNED ITSELF OFF: ", { L"DESLIGADO - DESLIGOU SOZINHO: ", L"DESACTIVADO - SE APAG\u00D3 SOLO: ", L"\u5173\u95ED - \u5DF2\u81EA\u52A8\u5173\u95ED\uFF1A", L"\u30AA\u30D5 - \u81EA\u52D5\u3067\u30AA\u30D5\uFF1A", L"\uAEBC\uC9D0 - \uC790\uB3D9\uC73C\uB85C \uAEBC\uC9D0: " } },
        { L"OFF (DEFAULT) - FPS PER CARD ONLY AT THE END OF A BOUNDED RUN", { L"DESLIGADO (PADR\u00C3O) - FPS POR PLACA S\u00D3 NO FIM DE UMA EXECU\u00C7\u00C3O LIMITADA", L"DESACTIVADO (PREDETERMINADO) - FPS POR TARJETA SOLO AL FINAL", L"\u5173\u95ED\uFF08\u9ED8\u8BA4\uFF09- \u4EC5\u5728\u9650\u5B9A\u8FD0\u884C\u7ED3\u675F\u65F6\u8BB0\u5F55\u6BCF\u5F20\u663E\u5361\u7684 FPS", L"\u30AA\u30D5\uFF08\u65E2\u5B9A\uFF09- \u30AB\u30FC\u30C9\u3054\u3068\u306E FPS \u306F\u533A\u5207\u308A\u5B9F\u884C\u306E\u7D42\u4E86\u6642\u306E\u307F", L"\uAEBC\uC9D0 (\uAE30\uBCF8\uAC12) - \uC81C\uD55C \uC2E4\uD589 \uC885\uB8CC \uC2DC\uC5D0\uB9CC \uCE74\uB4DC\uBCC4 FPS" } },
        { L"NOT RUN YET", { L"AINDA N\u00C3O EXECUTADO", L"A\u00DAN NO EJECUTADO", L"\u5C1A\u672A\u8FD0\u884C", L"\u672A\u5B9F\u884C", L"\uC544\uC9C1 \uC2E4\uD589 \uC548 \uD568" } },
        { L"SYNTHETIC RUN, NO GAME", { L"TESTE SINT\u00C9TICO, SEM JOGO", L"PRUEBA SINT\u00C9TICA, SIN JUEGO", L"\u5408\u6210\u6D4B\u8BD5\uFF0C\u65E0\u6E38\u620F", L"\u5408\u6210\u30C6\u30B9\u30C8\u3001\u30B2\u30FC\u30E0\u306A\u3057", L"\uD569\uC131 \uD14C\uC2A4\uD2B8, \uAC8C\uC784 \uC5C6\uC74C" } },
        { L"THE CARD COULD NOT RUN THE DLSS 5 MODEL - SEE NRBENCH_LOG.TXT", { L"A PLACA N\u00C3O CONSEGUIU EXECUTAR O MODELO DLSS 5 - VEJA NRBENCH_LOG.TXT", L"LA TARJETA NO PUDO EJECUTAR EL MODELO DLSS 5 - MIRA NRBENCH_LOG.TXT", L"\u8BE5\u663E\u5361\u65E0\u6CD5\u8FD0\u884C DLSS 5 \u6A21\u578B - \u89C1 NRBENCH_LOG.TXT", L"\u3053\u306E\u30AB\u30FC\u30C9\u3067\u306F DLSS 5 \u30E2\u30C7\u30EB\u3092\u5B9F\u884C\u3067\u304D\u307E\u305B\u3093 - NRBENCH_LOG.TXT \u3092\u53C2\u7167", L"\uC774 \uCE74\uB4DC\uC5D0\uC11C DLSS 5 \uBAA8\uB378\uC744 \uC2E4\uD589\uD560 \uC218 \uC5C6\uC74C - NRBENCH_LOG.TXT \uCC38\uACE0" } },
        { L"SYNTHETIC: ", { L"SINT\u00C9TICO: ", L"SINT\u00C9TICO: ", L"\u5408\u6210\u6D4B\u8BD5\uFF1A", L"\u5408\u6210\u30C6\u30B9\u30C8\uFF1A", L"\uD569\uC131 \uD14C\uC2A4\uD2B8: " } },
        { L" PASS  ", { L" PASSAGEM  ", L" PASADA  ", L" \u904D  ", L" \u30D1\u30B9  ", L" \uD328\uC2A4  " } },
        { L" PASSES  ", { L" PASSAGENS  ", L" PASADAS  ", L" \u904D  ", L" \u30D1\u30B9  ", L" \uD328\uC2A4  " } },
        { L" MS/FRAME  FITS ", { L" MS/QUADRO  AT\u00C9 ", L" MS/FOTOGRAMA  HASTA ", L" \u6BEB\u79D2/\u5E27  \u53EF\u8FBE ", L" MS/\u30D5\u30EC\u30FC\u30E0  \u6700\u5927 ", L" MS/\uD504\uB808\uC784  \uCD5C\uB300 " } },
        { L"NO LAUNCH RECORDED - NOTHING TO COMPARE YET", { L"NENHUMA EXECU\u00C7\u00C3O REGISTRADA - NADA PARA COMPARAR", L"NO HAY EJECUCIONES REGISTRADAS - NADA QUE COMPARAR", L"\u65E0\u542F\u52A8\u8BB0\u5F55 - \u6682\u65E0\u53EF\u6BD4\u8F83\u7684\u5185\u5BB9", L"\u8D77\u52D5\u8A18\u9332\u306A\u3057 - \u307E\u3060\u6BD4\u8F03\u3067\u304D\u307E\u305B\u3093", L"\uC2E4\uD589 \uAE30\uB85D \uC5C6\uC74C - \uC544\uC9C1 \uBE44\uAD50\uD560 \uAC83 \uC5C6\uC74C" } },
        { L"MAIN DISPLAY NOT FOUND - NOTHING TO COMPARE", { L"MONITOR PRINCIPAL N\u00C3O ENCONTRADO - NADA A COMPARAR", L"MONITOR PRINCIPAL NO ENCONTRADO - NADA QUE COMPARAR", L"\u672A\u627E\u5230\u4E3B\u663E\u793A\u5668 - \u65E0\u53EF\u5BF9\u6BD4", L"\u30E1\u30A4\u30F3\u30C7\u30A3\u30B9\u30D7\u30EC\u30A4\u304C\u898B\u3064\u304B\u308A\u307E\u305B\u3093 - \u6BD4\u8F03\u3067\u304D\u307E\u305B\u3093", L"\uAE30\uBCF8 \uBAA8\uB2C8\uD130\uB97C \uCC3E\uC744 \uC218 \uC5C6\uC74C - \uBE44\uAD50\uD560 \uAC83 \uC5C6\uC74C" } },
        { L"MISMATCH RESOLUTION FOUND", { L"RESOLU\u00C7\u00C3O DIFERENTE ENCONTRADA", L"SE ENCONTR\u00D3 UNA RESOLUCI\u00D3N DISTINTA", L"\u53D1\u73B0\u5206\u8FA8\u7387\u4E0D\u4E00\u81F4", L"\u89E3\u50CF\u5EA6\u306E\u4E0D\u4E00\u81F4\u304C\u3042\u308A\u307E\u3059", L"\uD574\uC0C1\uB3C4 \uBD88\uC77C\uCE58 \uBC1C\uACAC" } },
        { L"GAME RENDERED %uX%u, %s IS %uX%u", { L"O JOGO RENDERIZOU %uX%u, %s \u00C9 %uX%u", L"EL JUEGO RENDERIZ\u00D3 %uX%u, %s ES %uX%u", L"\u6E38\u620F\u6E32\u67D3 %uX%u\uFF0C%s \u4E3A %uX%u", L"\u30B2\u30FC\u30E0\u63CF\u753B %uX%u\u3001%s \u306F %uX%u", L"\uAC8C\uC784 \uB80C\uB354\uB9C1 %uX%u, %s: %uX%u" } },
        { L"TEST BORDERLESS OR FULLSCREEN (GAME DEPENDENT)", { L"TESTE SEM BORDAS OU TELA CHEIA (DEPENDE DO JOGO)", L"PRUEBA SIN BORDES O PANTALLA COMPLETA (SEG\u00DAN EL JUEGO)", L"\u8BF7\u6D4B\u8BD5\u65E0\u8FB9\u6846\u6216\u5168\u5C4F\uFF08\u53D6\u51B3\u4E8E\u6E38\u620F\uFF09", L"\u30DC\u30FC\u30C0\u30FC\u30EC\u30B9\u304B\u30D5\u30EB\u30B9\u30AF\u30EA\u30FC\u30F3\u3092\u8A66\u3057\u3066\u304F\u3060\u3055\u3044\uFF08\u30B2\u30FC\u30E0\u306B\u3088\u308B\uFF09", L"\uD14C\uB450\uB9AC \uC5C6\uC74C \uB610\uB294 \uC804\uCCB4 \uD654\uBA74\uC744 \uC2DC\uD5D8\uD558\uC138\uC694 (\uAC8C\uC784\uB9C8\uB2E4 \uB2E4\uB984)" } },
        { L"OK - GAME %uX%u ON %s %uX%u", { L"OK - JOGO %uX%u EM %s %uX%u", L"OK - JUEGO %uX%u EN %s %uX%u", L"\u6B63\u5E38 - \u6E38\u620F %uX%u\uFF0C%s %uX%u", L"OK - \u30B2\u30FC\u30E0 %uX%u\u3001%s %uX%u", L"\uC815\uC0C1 - \uAC8C\uC784 %uX%u, %s %uX%u" } },
        { L"FORCE FIT IS ON: THE PICTURE IS ALWAYS SCALED TO THE GAME WINDOW", { L"AJUSTE FOR\u00C7ADO LIGADO: A IMAGEM SEMPRE SE AJUSTA \u00C0 JANELA DO JOGO", L"AJUSTE FORZADO ACTIVADO: LA IMAGEN SIEMPRE SE ESCALA A LA VENTANA", L"\u5F3A\u5236\u9002\u914D\u5DF2\u5F00\u542F\uFF1A\u753B\u9762\u59CB\u7EC8\u7F29\u653E\u5230\u6E38\u620F\u7A97\u53E3", L"\u5F37\u5236\u30D5\u30A3\u30C3\u30C8 \u30AA\u30F3\uFF1A\u753B\u50CF\u3092\u5E38\u306B\u30B2\u30FC\u30E0\u30A6\u30A3\u30F3\u30C9\u30A6\u306B\u5408\u308F\u305B\u307E\u3059", L"\uAC15\uC81C \uB9DE\uCDA4 \uCF1C\uC9D0: \uD654\uBA74\uC744 \uD56D\uC0C1 \uAC8C\uC784 \uCC3D\uC5D0 \uB9DE\uCDA4" } },
        { L"BY DETECTION (DEFAULT)", { L"POR DETEC\u00C7\u00C3O (PADR\u00C3O)", L"POR DETECCI\u00D3N (PREDETERMINADO)", L"\u81EA\u52A8\u68C0\u6D4B\uFF08\u9ED8\u8BA4\uFF09", L"\u81EA\u52D5\u691C\u51FA\uFF08\u65E2\u5B9A\uFF09", L"\uC790\uB3D9 \uAC10\uC9C0 (\uAE30\uBCF8\uAC12)" } },
        { L"ALWAYS - NO DETECTION", { L"SEMPRE - SEM DETEC\u00C7\u00C3O", L"SIEMPRE - SIN DETECCI\u00D3N", L"\u59CB\u7EC8 - \u4E0D\u68C0\u6D4B", L"\u5E38\u306B - \u691C\u51FA\u306A\u3057", L"\uD56D\uC0C1 - \uAC10\uC9C0 \uC548 \uD568" } },
        { L"NEVER - THE OLD GATE", { L"NUNCA - A REGRA ANTIGA", L"NUNCA - LA REGLA ANTIGUA", L"\u4ECE\u4E0D - \u65E7\u89C4\u5219", L"\u3057\u306A\u3044 - \u65E7\u30EB\u30FC\u30EB", L"\uC548 \uD568 - \uC774\uC804 \uADDC\uCE59" } },
        { L"TURN OFF", { L"DESLIGAR", L"APAGAR", L"\u5173\u95ED", L"\u30AA\u30D5\u306B\u3059\u308B", L"\uB044\uAE30" } },
        { L"TURN ON", { L"LIGAR", L"ACTIVAR", L"\u5F00\u542F", L"\u30AA\u30F3\u306B\u3059\u308B", L"\uCF1C\uAE30" } },
        { L"FORCE FIT: ON", { L"AJUSTE FOR\u00C7ADO: LIGADO", L"AJUSTE FORZADO: ACTIVADO", L"\u5F3A\u5236\u9002\u914D\uFF1A\u5F00", L"\u5F37\u5236\u30D5\u30A3\u30C3\u30C8\uFF1A\u30AA\u30F3", L"\uAC15\uC81C \uB9DE\uCDA4: \uCF2C" } },
        { L"IF OUTPUT RESIZE IS NOT WORKING: FORCE FIT", { L"SE O REDIMENSIONAMENTO N\u00C3O FUNCIONAR: AJUSTE FOR\u00C7ADO", L"SI EL REESCALADO NO FUNCIONA: AJUSTE FORZADO", L"\u5982\u679C\u8F93\u51FA\u7F29\u653E\u65E0\u6548\uFF1A\u5F3A\u5236\u9002\u914D", L"\u51FA\u529B\u306E\u30EA\u30B5\u30A4\u30BA\u304C\u52B9\u304B\u306A\u3044\u5834\u5408\uFF1A\u5F37\u5236\u30D5\u30A3\u30C3\u30C8", L"\uCD9C\uB825 \uD06C\uAE30 \uC870\uC808\uC774 \uC548 \uB418\uBA74: \uAC15\uC81C \uB9DE\uCDA4" } },
        { L"UNKNOWN CARD", { L"PLACA DESCONHECIDA", L"TARJETA DESCONOCIDA", L"\u672A\u77E5\u663E\u5361", L"\u4E0D\u660E\u306A\u30AB\u30FC\u30C9", L"\uC54C \uC218 \uC5C6\uB294 \uCE74\uB4DC" } },
        { L"NOT IN THE DXGI TABLE", { L"FORA DA TABELA DXGI", L"NO EST\u00C1 EN LA TABLA DXGI", L"\u4E0D\u5728 DXGI \u5217\u8868\u4E2D", L"DXGI \u4E00\u89A7\u306B\u3042\u308A\u307E\u305B\u3093", L"DXGI \uBAA9\uB85D\uC5D0 \uC5C6\uC74C" } },
        { L"DLSS 5 - LAUNCH", { L"DLSS 5 - EXECU\u00C7\u00C3O", L"DLSS 5 - EJECUCI\u00D3N", L"DLSS 5 - \u542F\u52A8", L"DLSS 5 - \u8D77\u52D5", L"DLSS 5 - \uC2E4\uD589" } },
        { L"GAME - LAUNCH", { L"JOGO - EXECU\u00C7\u00C3O", L"JUEGO - EJECUCI\u00D3N", L"\u6E38\u620F - \u542F\u52A8", L"\u30B2\u30FC\u30E0 - \u8D77\u52D5", L"\uAC8C\uC784 - \uC2E4\uD589" } },
        { L"DLSS 5 - DIAGNOSE", { L"DLSS 5 - DIAGN\u00D3STICO", L"DLSS 5 - DIAGN\u00D3STICO", L"DLSS 5 - \u8BCA\u65AD", L"DLSS 5 - \u8A3A\u65AD", L"DLSS 5 - \uC9C4\uB2E8" } },
        { L"NO GRAPHICS ADAPTER FOUND", { L"NENHUM ADAPTADOR GR\u00C1FICO ENCONTRADO", L"NO SE ENCONTR\u00D3 NING\u00DAN ADAPTADOR GR\u00C1FICO", L"\u672A\u627E\u5230\u56FE\u5F62\u9002\u914D\u5668", L"\u30B0\u30E9\u30D5\u30A3\u30C3\u30AF\u30A2\u30C0\u30D7\u30BF\u30FC\u304C\u898B\u3064\u304B\u308A\u307E\u305B\u3093", L"\uADF8\uB798\uD53D \uC5B4\uB311\uD130\uB97C \uCC3E\uC744 \uC218 \uC5C6\uC74C" } },
        { L"OPTIONS  SAME KEYS THE RESHADE PANEL WRITES - NEXT LAUNCH", { L"OP\u00C7\u00D5ES  AS MESMAS CHAVES DO PAINEL DO RESHADE - PR\u00D3XIMA EXECU\u00C7\u00C3O", L"OPCIONES  LAS MISMAS CLAVES QUE EL PANEL DE RESHADE - PR\u00D3XIMA EJECUCI\u00D3N", L"\u9009\u9879  \u4E0E RESHADE \u9762\u677F\u76F8\u540C\u7684\u8BBE\u7F6E - \u4E0B\u6B21\u542F\u52A8\u751F\u6548", L"\u30AA\u30D7\u30B7\u30E7\u30F3  RESHADE \u30D1\u30CD\u30EB\u3068\u540C\u3058\u8A2D\u5B9A - \u6B21\u56DE\u306E\u8D77\u52D5\u304B\u3089", L"\uC635\uC158  RESHADE \uD328\uB110\uACFC \uAC19\uC740 \uC124\uC815 - \uB2E4\uC74C \uC2E4\uD589\uBD80\uD130" } },
        { L"TUNING", { L"AJUSTE", L"AJUSTE", L"\u8C03\u6821", L"\u8ABF\u6574", L"\uD29C\uB2DD" } },
        { L"ON - THE MODEL CONTROLS BELOW APPLY", { L"LIGADO - OS CONTROLES ABAIXO VALEM", L"ACTIVADO - SE APLICAN LOS CONTROLES DE ABAJO", L"\u5F00 - \u4E0B\u65B9\u6A21\u578B\u63A7\u5236\u751F\u6548", L"\u30AA\u30F3 - \u4E0B\u306E\u30E2\u30C7\u30EB\u8A2D\u5B9A\u304C\u6709\u52B9", L"\uCF2C - \uC544\uB798 \uBAA8\uB378 \uC124\uC815 \uC801\uC6A9" } },
        { L"OFF - MODEL DEFAULTS", { L"DESLIGADO - PADR\u00D5ES DO MODELO", L"DESACTIVADO - VALORES DEL MODELO", L"\u5173 - \u6A21\u578B\u9ED8\u8BA4\u503C", L"\u30AA\u30D5 - \u30E2\u30C7\u30EB\u65E2\u5B9A\u5024", L"\uB054 - \uBAA8\uB378 \uAE30\uBCF8\uAC12" } },
        { L"MODEL STYLE", { L"ESTILO DO MODELO", L"ESTILO DEL MODELO", L"\u6A21\u578B\u98CE\u683C", L"\u30E2\u30C7\u30EB\u30B9\u30BF\u30A4\u30EB", L"\uBAA8\uB378 \uC2A4\uD0C0\uC77C" } },
        { L"TONE", { L"TOM", L"TONO", L"\u8272\u8C03", L"\u30C8\u30FC\u30F3", L"\uD1A4" } },
        { L"STRUCTURE", { L"ESTRUTURA", L"ESTRUCTURA", L"\u7ED3\u6784", L"\u69CB\u9020", L"\uAD6C\uC870" } },
        { L"SKIN", { L"PELE", L"PIEL", L"\u76AE\u80A4", L"\u808C", L"\uD53C\uBD80" } },
        { L"AUTO MASK", { L"M\u00C1SCARA AUTOM\u00C1TICA", L"M\u00C1SCARA AUTOM\u00C1TICA", L"\u81EA\u52A8\u906E\u7F69", L"\u81EA\u52D5\u30DE\u30B9\u30AF", L"\uC790\uB3D9 \uB9C8\uC2A4\uD06C" } },
        { L"INTENSITY", { L"INTENSIDADE", L"INTENSIDAD", L"\u5F3A\u5EA6", L"\u5F37\u5EA6", L"\uAC15\uB3C4" } },
        { L"PASSES", { L"PASSAGENS", L"PASADAS", L"\u904D\u6570", L"\u30D1\u30B9\u6570", L"\uD328\uC2A4 \uC218" } },
        { L"FRAME GENERATION", { L"GERA\u00C7\u00C3O DE QUADROS", L"GENERACI\u00D3N DE FOTOGRAMAS", L"\u5E27\u751F\u6210", L"\u30D5\u30EC\u30FC\u30E0\u751F\u6210", L"\uD504\uB808\uC784 \uC0DD\uC131" } },
        { L"KEEP 1 GENERATED FRAME IN", { L"MANTER 1 GERADO A CADA", L"CONSERVAR 1 GENERADO CADA", L"\u6BCF\u51E0\u5E27\u4FDD\u7559 1 \u4E2A\u751F\u6210\u5E27", L"\u751F\u6210\u30D5\u30EC\u30FC\u30E0\u3092\u6B8B\u3059\u9593\u9694", L"\uC0DD\uC131 \uD504\uB808\uC784 1\uAC1C \uC720\uC9C0 \uAC04\uACA9" } },
        { L"DLSS ON GPU 1", { L"DLSS NA GPU 1", L"DLSS EN LA GPU 1", L"GPU 1 \u4E0A\u7684 DLSS", L"GPU 1 \u306E DLSS", L"GPU 1\uC758 DLSS" } },
        { L"UPSCALING", { L"UPSCALING", L"REESCALADO", L"\u8D85\u5206\u8FA8\u7387", L"\u30A2\u30C3\u30D7\u30B9\u30B1\u30FC\u30EA\u30F3\u30B0", L"\uC5C5\uC2A4\uCF00\uC77C\uB9C1" } },
        { L"DLSS MODE", { L"MODO DLSS", L"MODO DLSS", L"DLSS \u6A21\u5F0F", L"DLSS \u30E2\u30FC\u30C9", L"DLSS \uBAA8\uB4DC" } },
        { L"DLSS PRESET", { L"PRESET DLSS", L"PRESET DLSS", L"DLSS \u9884\u8BBE", L"DLSS \u30D7\u30EA\u30BB\u30C3\u30C8", L"DLSS \uD504\uB9AC\uC14B" } },
        { L"CROP", { L"RECORTE", L"RECORTE", L"\u88C1\u526A", L"\u30AF\u30ED\u30C3\u30D7", L"\uC790\uB974\uAE30" } },
        { L"16-BIT COLOUR", { L"COR DE 16 BITS", L"COLOR DE 16 BITS", L"16 \u4F4D\u8272\u5F69", L"16 \u30D3\u30C3\u30C8\u30AB\u30E9\u30FC", L"16\uBE44\uD2B8 \uC0C9\uC0C1" } },
        { L"16-BIT POWER", { L"POT\u00CANCIA 16 BITS", L"POTENCIA 16 BITS", L"16 \u4F4D\u5F3A\u5EA6", L"16 \u30D3\u30C3\u30C8\u4FC2\u6570", L"16\uBE44\uD2B8 \uACC4\uC218" } },
        { L"FRAME GENERATION NEEDS MVEC=3 (THE GAME'S OWN VECTORS).", { L"A GERA\u00C7\u00C3O DE QUADROS PRECISA DE MVEC=3 (OS VETORES DO JOGO).", L"LA GENERACI\u00D3N DE FOTOGRAMAS NECESITA MVEC=3 (LOS VECTORES DEL JUEGO).", L"\u5E27\u751F\u6210\u9700\u8981 MVEC=3\uFF08\u6E38\u620F\u81EA\u8EAB\u7684\u8FD0\u52A8\u77E2\u91CF\uFF09\u3002", L"\u30D5\u30EC\u30FC\u30E0\u751F\u6210\u306B\u306F MVEC=3\uFF08\u30B2\u30FC\u30E0\u81EA\u8EAB\u306E\u30D9\u30AF\u30C8\u30EB\uFF09\u304C\u5FC5\u8981\u3067\u3059\u3002", L"\uD504\uB808\uC784 \uC0DD\uC131\uC5D0\uB294 MVEC=3 (\uAC8C\uC784 \uC790\uCCB4 \uBCA1\uD130)\uC774 \uD544\uC694\uD569\uB2C8\uB2E4." } },
        { L"FRAME GENERATION SETTINGS MAY CRASH THE GAME AT STARTUP.", { L"AS OP\u00C7\u00D5ES DE GERA\u00C7\u00C3O DE QUADROS PODEM TRAVAR O JOGO AO INICIAR.", L"LAS OPCIONES DE GENERACI\u00D3N DE FOTOGRAMAS PUEDEN CERRAR EL JUEGO AL INICIAR.", L"\u5E27\u751F\u6210\u8BBE\u7F6E\u53EF\u80FD\u5BFC\u81F4\u6E38\u620F\u542F\u52A8\u65F6\u5D29\u6E83\u3002", L"\u30D5\u30EC\u30FC\u30E0\u751F\u6210\u306E\u8A2D\u5B9A\u306F\u8D77\u52D5\u6642\u306B\u30B2\u30FC\u30E0\u304C\u30AF\u30E9\u30C3\u30B7\u30E5\u3059\u308B\u5834\u5408\u304C\u3042\u308A\u307E\u3059\u3002", L"\uD504\uB808\uC784 \uC0DD\uC131 \uC124\uC815\uC740 \uC2DC\uC791 \uC2DC \uAC8C\uC784\uC774 \uCDA9\uB3CC\uD560 \uC218 \uC788\uC2B5\uB2C8\uB2E4." } },
        { L"DLSS ON GPU 1: OFF IS THE BETTER PICTURE, ON IS CHEAPER.", { L"DLSS NA GPU 1: DESLIGADO D\u00C1 IMAGEM MELHOR, LIGADO \u00C9 MAIS LEVE.", L"DLSS EN LA GPU 1: DESACTIVADO DA MEJOR IMAGEN, ACTIVADO ES M\u00C1S LIGERO.", L"GPU 1 \u4E0A\u7684 DLSS\uFF1A\u5173\u95ED\u753B\u8D28\u66F4\u597D\uFF0C\u5F00\u542F\u66F4\u7701\u8D44\u6E90\u3002", L"GPU 1 \u306E DLSS\uFF1A\u30AA\u30D5\u306F\u753B\u8CEA\u304C\u4E0A\u3001\u30AA\u30F3\u306F\u8CA0\u8377\u304C\u8EFD\u3044\u3002", L"GPU 1\uC758 DLSS: \uB044\uBA74 \uD654\uC9C8\uC774 \uC88B\uACE0, \uCF1C\uBA74 \uBD80\uB2F4\uC774 \uC801\uC74C." } },
        { L"INSTALL GUIDE", { L"GUIA DE INSTALA\u00C7\u00C3O", L"GU\u00CDA DE INSTALACI\u00D3N", L"\u5B89\u88C5\u6307\u5357", L"\u30A4\u30F3\u30B9\u30C8\u30FC\u30EB\u30AC\u30A4\u30C9", L"\uC124\uCE58 \uAC00\uC774\uB4DC" } },
        { L"Install ReShade with add-on support.", { L"INSTALE O RESHADE COM SUPORTE A ADD-ONS.", L"INSTALA RESHADE CON SOPORTE PARA ADD-ONS.", L"\u5B89\u88C5\u652F\u6301\u63D2\u4EF6\u7684 ReShade\u3002", L"\u30A2\u30C9\u30AA\u30F3\u5BFE\u5FDC\u7248\u306E ReShade \u3092\u30A4\u30F3\u30B9\u30C8\u30FC\u30EB\u3057\u3066\u304F\u3060\u3055\u3044\u3002", L"\uC560\uB4DC\uC628\uC744 \uC9C0\uC6D0\uD558\uB294 ReShade\uB97C \uC124\uCE58\uD558\uC138\uC694." } },
        { L"Get nvngx_dlssnr.dll. I do not distribute this file. You can get it from RHI (github.com/RankFTW/RHI) or from the RenoDX Discord.", { L"OBTENHA O NVNGX_DLSSNR.DLL. EU N\u00C3O DISTRIBUO ESTE ARQUIVO. VOC\u00CA PODE OBT\u00CA-LO NO RHI (GITHUB.COM/RANKFTW/RHI) OU NO DISCORD DO RENODX.", L"CONSIGUE NVNGX_DLSSNR.DLL. YO NO DISTRIBUYO ESTE ARCHIVO. PUEDES CONSEGUIRLO EN RHI (GITHUB.COM/RANKFTW/RHI) O EN EL DISCORD DE RENODX.", L"\u83B7\u53D6 nvngx_dlssnr.dll\u3002\u6211\u4E0D\u5206\u53D1\u6B64\u6587\u4EF6\u3002\u53EF\u4EE5\u4ECE RHI\uFF08github.com/RankFTW/RHI\uFF09\u6216 RenoDX \u7684 Discord \u83B7\u53D6\u3002", L"nvngx_dlssnr.dll \u3092\u5165\u624B\u3057\u3066\u304F\u3060\u3055\u3044\u3002\u3053\u306E\u30D5\u30A1\u30A4\u30EB\u306F\u79C1\u304B\u3089\u306F\u914D\u5E03\u3057\u3066\u3044\u307E\u305B\u3093\u3002RHI\uFF08github.com/RankFTW/RHI\uFF09\u307E\u305F\u306F RenoDX \u306E Discord \u304B\u3089\u5165\u624B\u3067\u304D\u307E\u3059\u3002", L"nvngx_dlssnr.dll\uC744 \uBC1B\uC73C\uC138\uC694. \uC800\uB294 \uC774 \uD30C\uC77C\uC744 \uBC30\uD3EC\uD558\uC9C0 \uC54A\uC2B5\uB2C8\uB2E4. RHI(github.com/RankFTW/RHI) \uB610\uB294 RenoDX \uB514\uC2A4\uCF54\uB4DC\uC5D0\uC11C \uBC1B\uC744 \uC218 \uC788\uC2B5\uB2C8\uB2E4." } },
        { L"Put nvngx_dlssnr.dll in the mgpu folder inside the game folder.", { L"COLOQUE O NVNGX_DLSSNR.DLL NA PASTA MGPU DENTRO DA PASTA DO JOGO.", L"PON NVNGX_DLSSNR.DLL EN LA CARPETA MGPU DENTRO DE LA CARPETA DEL JUEGO.", L"\u5C06 nvngx_dlssnr.dll \u653E\u5165\u6E38\u620F\u6587\u4EF6\u5939\u4E2D\u7684 mgpu \u6587\u4EF6\u5939\u3002", L"nvngx_dlssnr.dll \u3092\u30B2\u30FC\u30E0\u30D5\u30A9\u30EB\u30C0\u30FC\u5185\u306E mgpu \u30D5\u30A9\u30EB\u30C0\u30FC\u306B\u7F6E\u3044\u3066\u304F\u3060\u3055\u3044\u3002", L"nvngx_dlssnr.dll\uC744 \uAC8C\uC784 \uD3F4\uB354 \uC548\uC758 mgpu \uD3F4\uB354\uC5D0 \uB123\uC73C\uC138\uC694." } },
        { L"Press COPY FILES on the Status page. It copies the add-on and its settings files.", { L"CLIQUE EM COPIAR ARQUIVOS NA P\u00C1GINA STATUS. ELE COPIA O ADD-ON E SEUS ARQUIVOS DE CONFIGURA\u00C7\u00C3O.", L"PULSA COPIAR ARCHIVOS EN LA P\u00C1GINA ESTADO. COPIA EL ADD-ON Y SUS ARCHIVOS DE CONFIGURACI\u00D3N.", L"\u5728\u201C\u72B6\u6001\u201D\u9875\u9762\u70B9\u51FB\u201C\u590D\u5236\u6587\u4EF6\u201D\u3002\u5B83\u4F1A\u590D\u5236\u63D2\u4EF6\u53CA\u5176\u8BBE\u7F6E\u6587\u4EF6\u3002", L"\u300C\u72B6\u614B\u300D\u30DA\u30FC\u30B8\u3067\u300C\u30D5\u30A1\u30A4\u30EB\u3092\u30B3\u30D4\u30FC\u300D\u3092\u62BC\u3057\u3066\u304F\u3060\u3055\u3044\u3002\u30A2\u30C9\u30AA\u30F3\u3068\u8A2D\u5B9A\u30D5\u30A1\u30A4\u30EB\u3092\u30B3\u30D4\u30FC\u3057\u307E\u3059\u3002", L"\uC0C1\uD0DC \uD398\uC774\uC9C0\uC5D0\uC11C \uD30C\uC77C \uBCF5\uC0AC\uB97C \uB204\uB974\uC138\uC694. \uC560\uB4DC\uC628\uACFC \uC124\uC815 \uD30C\uC77C\uC744 \uBCF5\uC0AC\uD569\uB2C8\uB2E4." } },
        { L"Move any other ReShade add-ons (.addon64 or .addon files) out of the game folder. The Status page lists the ones it finds.", { L"TIRE DA PASTA DO JOGO QUALQUER OUTRO ADD-ON DO RESHADE (ARQUIVOS .ADDON64 OU .ADDON). A P\u00C1GINA STATUS LISTA OS QUE ENCONTRAR.", L"SACA DE LA CARPETA DEL JUEGO CUALQUIER OTRO ADD-ON DE RESHADE (ARCHIVOS .ADDON64 O .ADDON). LA P\u00C1GINA ESTADO MUESTRA LOS QUE ENCUENTRA.", L"\u5C06\u5176\u4ED6 ReShade \u63D2\u4EF6\uFF08.addon64 \u6216 .addon \u6587\u4EF6\uFF09\u79FB\u51FA\u6E38\u620F\u6587\u4EF6\u5939\u3002\u201C\u72B6\u6001\u201D\u9875\u9762\u4F1A\u5217\u51FA\u627E\u5230\u7684\u63D2\u4EF6\u3002", L"\u4ED6\u306E ReShade \u30A2\u30C9\u30AA\u30F3\uFF08.addon64 / .addon \u30D5\u30A1\u30A4\u30EB\uFF09\u306F\u30B2\u30FC\u30E0\u30D5\u30A9\u30EB\u30C0\u30FC\u306E\u5916\u3078\u79FB\u3057\u3066\u304F\u3060\u3055\u3044\u3002\u300C\u72B6\u614B\u300D\u30DA\u30FC\u30B8\u306B\u898B\u3064\u304B\u3063\u305F\u3082\u306E\u304C\u8868\u793A\u3055\u308C\u307E\u3059\u3002", L"\uB2E4\uB978 ReShade \uC560\uB4DC\uC628(.addon64 \uB610\uB294 .addon \uD30C\uC77C)\uC740 \uAC8C\uC784 \uD3F4\uB354 \uBC16\uC73C\uB85C \uC62E\uAE30\uC138\uC694. \uC0C1\uD0DC \uD398\uC774\uC9C0\uC5D0 \uCC3E\uC740 \uC560\uB4DC\uC628\uC774 \uD45C\uC2DC\uB429\uB2C8\uB2E4." } },
        { L"LIBRARY", { L"BIBLIOTECA", L"BIBLIOTECA", L"\u6E38\u620F\u5E93", L"\u30E9\u30A4\u30D6\u30E9\u30EA", L"\uB77C\uC774\uBE0C\uB7EC\uB9AC" } },
        { L"ADD THE GAME FOLDER:", { L"ADICIONE A PASTA DO JOGO:", L"A\u00D1ADE LA CARPETA DEL JUEGO:", L"\u6DFB\u52A0\u6E38\u620F\u6587\u4EF6\u5939\uFF1A", L"\u30B2\u30FC\u30E0\u30D5\u30A9\u30EB\u30C0\u30FC\u3092\u8FFD\u52A0\uFF1A", L"\uAC8C\uC784 \uD3F4\uB354\uB97C \uCD94\uAC00\uD558\uC138\uC694:" } },
        { L"THE ONE WITH THE GAME'S EXE", { L"A QUE TEM O EXE DO JOGO", L"LA QUE TIENE EL EXE DEL JUEGO", L"\u5305\u542B\u6E38\u620F EXE \u7684\u6587\u4EF6\u5939", L"\u30B2\u30FC\u30E0\u306E EXE \u304C\u3042\u308B\u5834\u6240", L"\uAC8C\uC784 EXE\uAC00 \uC788\uB294 \uD3F4\uB354" } },
        { L"AND RESHADE IN IT", { L"E O RESHADE", L"Y RESHADE", L"\u5E76\u5DF2\u5B89\u88C5 RESHADE", L"RESHADE \u3082\u5165\u3063\u3066\u3044\u308B\u5834\u6240", L"RESHADE\uB3C4 \uC788\uB294 \uD3F4\uB354" } },
        { L"NO GAME SELECTED", { L"NENHUM JOGO SELECIONADO", L"NING\u00DAN JUEGO SELECCIONADO", L"\u672A\u9009\u62E9\u6E38\u620F", L"\u30B2\u30FC\u30E0\u304C\u9078\u629E\u3055\u308C\u3066\u3044\u307E\u305B\u3093", L"\uC120\uD0DD\uB41C \uAC8C\uC784 \uC5C6\uC74C" } },
        { L"LAST LAUNCH  ", { L"\u00DALTIMA EXECU\u00C7\u00C3O  ", L"\u00DALTIMA EJECUCI\u00D3N  ", L"\u4E0A\u6B21\u542F\u52A8  ", L"\u524D\u56DE\u306E\u8D77\u52D5  ", L"\uB9C8\uC9C0\uB9C9 \uC2E4\uD589  " } },
        { L"  THE GAME RUN, RECORDED BY THE ADD-ON", { L"  REGISTRADA PELO ADD-ON", L"  REGISTRADA POR EL ADD-ON", L"  \u7531\u63D2\u4EF6\u8BB0\u5F55", L"  \u30A2\u30C9\u30AA\u30F3\u304C\u8A18\u9332", L"  \uC560\uB4DC\uC628\uC774 \uAE30\uB85D" } },
        { L"LAST LAUNCH", { L"\u00DALTIMA EXECU\u00C7\u00C3O", L"\u00DALTIMA EJECUCI\u00D3N", L"\u4E0A\u6B21\u542F\u52A8", L"\u524D\u56DE\u306E\u8D77\u52D5", L"\uB9C8\uC9C0\uB9C9 \uC2E4\uD589" } },
        { L"GAME", { L"JOGO", L"JUEGO", L"\u6E38\u620F", L"\u30B2\u30FC\u30E0", L"\uAC8C\uC784" } },
        { L"BOTTLENECK", { L"GARGALO", L"CUELLO DE BOTELLA", L"\u74F6\u9888", L"\u30DC\u30C8\u30EB\u30CD\u30C3\u30AF", L"\uBCD1\uBAA9" } },
        { L"DISPLAY", { L"MONITOR", L"MONITOR", L"\u663E\u793A\u5668", L"\u30C7\u30A3\u30B9\u30D7\u30EC\u30A4", L"\uBAA8\uB2C8\uD130" } },
        { L"LAYOUT  CABLES READ FROM THIS PC NOW, ROLES FROM THE LAUNCH", { L"LAYOUT  CABOS LIDOS DESTE PC AGORA, FUN\u00C7\u00D5ES DA EXECU\u00C7\u00C3O", L"DISPOSICI\u00D3N  CABLES DE ESTE PC AHORA, FUNCIONES DE LA EJECUCI\u00D3N", L"\u5E03\u5C40  \u7EBF\u7F06\u4E3A\u6B64\u7535\u8111\u5F53\u524D\u72B6\u6001\uFF0C\u89D2\u8272\u6765\u81EA\u542F\u52A8\u8BB0\u5F55", L"\u69CB\u6210  \u30B1\u30FC\u30D6\u30EB\u306F\u73FE\u5728\u306E PC \u304B\u3089\u3001\u5F79\u5272\u306F\u8D77\u52D5\u8A18\u9332\u304B\u3089", L"\uAD6C\uC131  \uCF00\uC774\uBE14\uC740 \uD604\uC7AC PC\uC5D0\uC11C, \uC5ED\uD560\uC740 \uC2E4\uD589 \uAE30\uB85D\uC5D0\uC11C" } },
        { L"INSTALL", { L"INSTALAR", L"INSTALAR", L"\u5B89\u88C5", L"\u30A4\u30F3\u30B9\u30C8\u30FC\u30EB", L"\uC124\uCE58" } },
        { L"DIAGNOSE", { L"DIAGN\u00D3STICO", L"DIAGN\u00D3STICO", L"\u8BCA\u65AD", L"\u8A3A\u65AD", L"\uC9C4\uB2E8" } },
        { L"RESOLUTION", { L"RESOLU\u00C7\u00C3O", L"RESOLUCI\u00D3N", L"\u5206\u8FA8\u7387", L"\u89E3\u50CF\u5EA6", L"\uD574\uC0C1\uB3C4" } },
        { L"MULTI-DISPLAY", { L"MULTIMONITOR", L"MULTIMONITOR", L"\u591A\u663E\u793A\u5668", L"\u30DE\u30EB\u30C1\u30C7\u30A3\u30B9\u30D7\u30EC\u30A4", L"\uB2E4\uC911 \uBAA8\uB2C8\uD130" } },
        { L"LAUNCH RECORD", { L"REGISTRO", L"REGISTRO", L"\u542F\u52A8\u8BB0\u5F55", L"\u8D77\u52D5\u8A18\u9332", L"\uC2E4\uD589 \uAE30\uB85D" } },
        { L"+ ADD GAME", { L"+ ADICIONAR JOGO", L"+ A\u00D1ADIR JUEGO", L"+ \u6DFB\u52A0\u6E38\u620F", L"+ \u30B2\u30FC\u30E0\u8FFD\u52A0", L"+ \uAC8C\uC784 \uCD94\uAC00" } },
        { L"REMOVE", { L"REMOVER", L"QUITAR", L"\u79FB\u9664", L"\u524A\u9664", L"\uC81C\uAC70" } },
        { L"LOGS", { L"LOGS", L"REGISTROS", L"\u65E5\u5FD7", L"\u30ED\u30B0", L"\uB85C\uADF8" } },
        { L"SCAN DISPLAY", { L"ESCANEAR MONITOR", L"ESCANEAR MONITOR", L"\u626B\u63CF\u663E\u793A\u5668", L"\u30C7\u30A3\u30B9\u30D7\u30EC\u30A4\u3092\u30B9\u30AD\u30E3\u30F3", L"\uBAA8\uB2C8\uD130 \uC2A4\uCE94" } },
        { L"1 PASS", { L"1 PASSAGEM", L"1 PASADA", L"1 \u904D", L"1 \u30D1\u30B9", L"1 \uD328\uC2A4" } },
        { L"2 PASSES", { L"2 PASSAGENS", L"2 PASADAS", L"2 \u904D", L"2 \u30D1\u30B9", L"2 \uD328\uC2A4" } },
        { L"STATUS", { L"STATUS", L"ESTADO", L"\u72B6\u6001", L"\u72B6\u614B", L"\uC0C1\uD0DC" } },
        { L"OPEN RESHADE DOWNLOAD", { L"ABRIR DOWNLOAD DO RESHADE", L"ABRIR DESCARGA DE RESHADE", L"\u6253\u5F00 RESHADE \u4E0B\u8F7D", L"RESHADE \u306E\u30C0\u30A6\u30F3\u30ED\u30FC\u30C9\u3092\u958B\u304F", L"RESHADE \uB2E4\uC6B4\uB85C\uB4DC \uC5F4\uAE30" } },
        { L"OPEN RHI", { L"ABRIR RHI", L"ABRIR RHI", L"\u6253\u5F00 RHI", L"RHI \u3092\u958B\u304F", L"RHI \uC5F4\uAE30" } },
        { L"OFF", { L"DESL.", L"NO", L"\u5173", L"\u30AA\u30D5", L"\uB054" } },
        { L"ON", { L"LIG.", L"S\u00CD", L"\u5F00", L"\u30AA\u30F3", L"\uCF2C" } },
        { L"MAP ONLY", { L"S\u00D3 MAPA", L"SOLO MAPA", L"\u4EC5\u6620\u5C04", L"\u30DE\u30C3\u30D7\u306E\u307F", L"\uB9F5\uB9CC" } },
        { L"FIX VECTORS", { L"CORRIGIR VETORES", L"CORREGIR VECTORES", L"\u4FEE\u6B63\u77E2\u91CF", L"\u30D9\u30AF\u30C8\u30EB\u4FEE\u6B63", L"\uBCA1\uD130 \uBCF4\uC815" } },
        { L"FIX + THIN", { L"CORRIGIR + REDUZIR", L"CORREGIR + REDUCIR", L"\u4FEE\u6B63 + \u62BD\u5E27", L"\u4FEE\u6B63 + \u9593\u5F15\u304D", L"\uBCF4\uC815 + \uC18E\uAE30" } },
        { L"NATIVE UPSCALING", { L"UPSCALING NATIVO", L"REESCALADO NATIVO", L"\u539F\u751F\u8D85\u5206", L"\u30CD\u30A4\u30C6\u30A3\u30D6\u30A2\u30C3\u30D7\u30B9\u30B1\u30FC\u30EB", L"\uB124\uC774\uD2F0\uBE0C \uC5C5\uC2A4\uCF00\uC77C" } },
        { L"EXPERIMENTAL", { L"EXPERIMENTAL", L"EXPERIMENTAL", L"\u5B9E\u9A8C\u6027", L"\u5B9F\u9A13\u7684", L"\uC2E4\uD5D8\uC801" } },
        { L"QUALITY", { L"QUALIDADE", L"CALIDAD", L"\u8D28\u91CF", L"\u54C1\u8CEA", L"\uD488\uC9C8" } },
        { L"BALANCED", { L"EQUILIBRADO", L"EQUILIBRADO", L"\u5E73\u8861", L"\u30D0\u30E9\u30F3\u30B9", L"\uADE0\uD615" } },
        { L"PERFORMANCE", { L"DESEMPENHO", L"RENDIMIENTO", L"\u6027\u80FD", L"\u30D1\u30D5\u30A9\u30FC\u30DE\u30F3\u30B9", L"\uC131\uB2A5" } },
        { L"TITLE DEFAULT", { L"PADR\u00C3O DO JOGO", L"PREDETERMINADO", L"\u6E38\u620F\u9ED8\u8BA4", L"\u30B2\u30FC\u30E0\u65E2\u5B9A", L"\uAC8C\uC784 \uAE30\uBCF8\uAC12" } },
        { L"AUTO CROP", { L"RECORTE AUTOM\u00C1TICO", L"RECORTE AUTOM\u00C1TICO", L"\u81EA\u52A8\u88C1\u526A", L"\u81EA\u52D5\u30AF\u30ED\u30C3\u30D7", L"\uC790\uB3D9 \uC790\uB974\uAE30" } },
        { L"RUNNING..", { L"EXECUTANDO..", L"EJECUTANDO..", L"\u8FD0\u884C\u4E2D..", L"\u5B9F\u884C\u4E2D..", L"\uC2E4\uD589 \uC911.." } },
        { L"NRCHECK.EXE COULD NOT BE STARTED", { L"N\u00C3O FOI POSS\u00CDVEL INICIAR O NRCHECK.EXE", L"NO SE PUDO INICIAR NRCHECK.EXE", L"\u65E0\u6CD5\u542F\u52A8 NRCHECK.EXE", L"NRCHECK.EXE \u3092\u8D77\u52D5\u3067\u304D\u307E\u305B\u3093\u3067\u3057\u305F", L"NRCHECK.EXE\uB97C \uC2DC\uC791\uD560 \uC218 \uC5C6\uC74C" } },
        { L"FORCE FIT", { L"AJUSTE FOR\u00C7ADO", L"AJUSTE FORZADO", L"\u5F3A\u5236\u9002\u914D", L"\u5F37\u5236\u30D5\u30A3\u30C3\u30C8", L"\uAC15\uC81C \uB9DE\uCDA4" } },
        { L"FORCE FIT - NOT TESTED", { L"AJUSTE FOR\u00C7ADO - N\u00C3O TESTADO", L"AJUSTE FORZADO - SIN PROBAR", L"\u5F3A\u5236\u9002\u914D - \u672A\u6D4B\u8BD5", L"\u5F37\u5236\u30D5\u30A3\u30C3\u30C8 - \u672A\u691C\u8A3C", L"\uAC15\uC81C \uB9DE\uCDA4 - \uD14C\uC2A4\uD2B8 \uC548 \uB428" } },
        { L"This game has no mgpu.ini yet. Install the bridge first.", { L"Este jogo ainda n\u00E3o tem mgpu.ini. Instale o bridge primeiro.", L"Este juego a\u00FAn no tiene mgpu.ini. Instala primero el bridge.", L"\u6B64\u6E38\u620F\u8FD8\u6CA1\u6709 mgpu.ini\u3002\u8BF7\u5148\u5B89\u88C5 bridge\u3002", L"\u3053\u306E\u30B2\u30FC\u30E0\u306B\u306F\u307E\u3060 mgpu.ini \u304C\u3042\u308A\u307E\u305B\u3093\u3002\u5148\u306B bridge \u3092\u30A4\u30F3\u30B9\u30C8\u30FC\u30EB\u3057\u3066\u304F\u3060\u3055\u3044\u3002", L"\uC774 \uAC8C\uC784\uC5D0\uB294 \uC544\uC9C1 mgpu.ini\uAC00 \uC5C6\uC2B5\uB2C8\uB2E4. \uBA3C\uC800 bridge\uB97C \uC124\uCE58\uD558\uC138\uC694." } },
        { L"mgpu.ini could not be written.", { L"N\u00E3o foi poss\u00EDvel gravar o mgpu.ini.", L"No se pudo escribir mgpu.ini.", L"\u65E0\u6CD5\u5199\u5165 mgpu.ini\u3002", L"mgpu.ini \u3092\u66F8\u304D\u8FBC\u3081\u307E\u305B\u3093\u3067\u3057\u305F\u3002", L"mgpu.ini\uB97C \uC4F8 \uC218 \uC5C6\uC2B5\uB2C8\uB2E4." } },
        { L"The launcher's payload\\ folder is missing. Put the bridge release files in it (the add-on, mgpu.ini, gpu1.ini, ReShade2.ini, README.txt, LICENSE).", { L"A pasta payload\\ do launcher n\u00E3o existe. Coloque nela os arquivos do bridge (o add-on, mgpu.ini, gpu1.ini, ReShade2.ini, README.txt, LICENSE).", L"Falta la carpeta payload\\ del launcher. Pon en ella los archivos del bridge (el add-on, mgpu.ini, gpu1.ini, ReShade2.ini, README.txt, LICENSE).", L"\u542F\u52A8\u5668\u7684 payload\\ \u6587\u4EF6\u5939\u4E0D\u5B58\u5728\u3002\u8BF7\u628A bridge \u7684\u53D1\u5E03\u6587\u4EF6\u653E\u5165\u5176\u4E2D\uFF08\u63D2\u4EF6\u3001mgpu.ini\u3001gpu1.ini\u3001ReShade2.ini\u3001README.txt\u3001LICENSE\uFF09\u3002", L"\u30E9\u30F3\u30C1\u30E3\u30FC\u306E payload\\ \u30D5\u30A9\u30EB\u30C0\u30FC\u304C\u3042\u308A\u307E\u305B\u3093\u3002bridge \u306E\u30EA\u30EA\u30FC\u30B9\u30D5\u30A1\u30A4\u30EB\uFF08\u30A2\u30C9\u30AA\u30F3\u3001mgpu.ini\u3001gpu1.ini\u3001ReShade2.ini\u3001README.txt\u3001LICENSE\uFF09\u3092\u5165\u308C\u3066\u304F\u3060\u3055\u3044\u3002", L"\uB7F0\uCC98\uC758 payload\\ \uD3F4\uB354\uAC00 \uC5C6\uC2B5\uB2C8\uB2E4. bridge \uBC30\uD3EC \uD30C\uC77C(\uC560\uB4DC\uC628, mgpu.ini, gpu1.ini, ReShade2.ini, README.txt, LICENSE)\uC744 \uB123\uC73C\uC138\uC694." } },
        { L"ReShade was not found in this folder. The bridge is a ReShade add-on and needs ReShade (with add-on support) installed first.\n\nCopy the bridge files anyway?", { L"O ReShade n\u00E3o foi encontrado nesta pasta. O bridge \u00E9 um add-on do ReShade e precisa do ReShade (com suporte a add-ons) instalado antes.\n\nCopiar os arquivos do bridge mesmo assim?", L"No se encontr\u00F3 ReShade en esta carpeta. El bridge es un add-on de ReShade y necesita ReShade (con soporte para add-ons) instalado antes.\n\n\u00BFCopiar los archivos del bridge de todos modos?", L"\u6B64\u6587\u4EF6\u5939\u4E2D\u672A\u627E\u5230 ReShade\u3002bridge \u662F ReShade \u63D2\u4EF6\uFF0C\u9700\u8981\u5148\u5B89\u88C5\u652F\u6301\u63D2\u4EF6\u7684 ReShade\u3002\n\n\u4ECD\u8981\u590D\u5236 bridge \u6587\u4EF6\u5417\uFF1F", L"\u3053\u306E\u30D5\u30A9\u30EB\u30C0\u30FC\u306B ReShade \u304C\u898B\u3064\u304B\u308A\u307E\u305B\u3093\u3002bridge \u306F ReShade \u306E\u30A2\u30C9\u30AA\u30F3\u306A\u306E\u3067\u3001\u5148\u306B\u30A2\u30C9\u30AA\u30F3\u5BFE\u5FDC\u7248\u306E ReShade \u304C\u5FC5\u8981\u3067\u3059\u3002\n\n\u305D\u308C\u3067\u3082 bridge \u306E\u30D5\u30A1\u30A4\u30EB\u3092\u30B3\u30D4\u30FC\u3057\u307E\u3059\u304B\uFF1F", L"\uC774 \uD3F4\uB354\uC5D0\uC11C ReShade\uB97C \uCC3E\uC744 \uC218 \uC5C6\uC2B5\uB2C8\uB2E4. bridge\uB294 ReShade \uC560\uB4DC\uC628\uC774\uBBC0\uB85C \uC560\uB4DC\uC628\uC744 \uC9C0\uC6D0\uD558\uB294 ReShade\uAC00 \uBA3C\uC800 \uC124\uCE58\uB418\uC5B4 \uC788\uC5B4\uC57C \uD569\uB2C8\uB2E4.\n\n\uADF8\uB798\uB3C4 bridge \uD30C\uC77C\uC744 \uBCF5\uC0AC\uD560\uAE4C\uC694?" } },
        { L"missing in payload: ", { L"faltando no payload: ", L"falta en payload: ", L"payload \u4E2D\u7F3A\u5C11\uFF1A", L"payload \u306B\u3042\u308A\u307E\u305B\u3093\uFF1A", L"payload\uC5D0 \uC5C6\uC74C: " } },
        { L"could not copy: ", { L"n\u00E3o foi poss\u00EDvel copiar: ", L"no se pudo copiar: ", L"\u65E0\u6CD5\u590D\u5236\uFF1A", L"\u30B3\u30D4\u30FC\u3067\u304D\u307E\u305B\u3093\uFF1A", L"\uBCF5\uC0AC\uD560 \uC218 \uC5C6\uC74C: " } },
        { L"\r\nnvngx_dlssnr.dll is not in the game's mgpu folder. It is not included with the bridge. You can get it from RHI (github.com/RankFTW/RHI) or from the RenoDX Discord. Put it in the mgpu folder. See the INSTALL page, steps 2 and 3.\r\n", { L"\r\nO nvngx_dlssnr.dll n\u00E3o est\u00E1 na pasta mgpu do jogo. Ele n\u00E3o vem com o bridge. Voc\u00EA pode obt\u00EA-lo no RHI (github.com/RankFTW/RHI) ou no Discord do RenoDX. Coloque-o na pasta mgpu. Veja a p\u00E1gina INSTALAR, passos 2 e 3.\r\n", L"\r\nnvngx_dlssnr.dll no est\u00E1 en la carpeta mgpu del juego. No se incluye con el bridge. Puedes conseguirlo en RHI (github.com/RankFTW/RHI) o en el Discord de RenoDX. Ponlo en la carpeta mgpu. Mira la p\u00E1gina INSTALAR, pasos 2 y 3.\r\n", L"\r\n\u6E38\u620F\u7684 mgpu \u6587\u4EF6\u5939\u4E2D\u6CA1\u6709 nvngx_dlssnr.dll\u3002bridge \u4E0D\u5305\u542B\u6B64\u6587\u4EF6\u3002\u53EF\u4EE5\u4ECE RHI\uFF08github.com/RankFTW/RHI\uFF09\u6216 RenoDX \u7684 Discord \u83B7\u53D6\u3002\u8BF7\u653E\u5165 mgpu \u6587\u4EF6\u5939\u3002\u89C1\u201C\u5B89\u88C5\u201D\u9875\u9762\u7B2C 2\u30013 \u6B65\u3002\r\n", L"\r\n\u30B2\u30FC\u30E0\u306E mgpu \u30D5\u30A9\u30EB\u30C0\u30FC\u306B nvngx_dlssnr.dll \u304C\u3042\u308A\u307E\u305B\u3093\u3002bridge \u306B\u306F\u542B\u307E\u308C\u3066\u3044\u307E\u305B\u3093\u3002RHI\uFF08github.com/RankFTW/RHI\uFF09\u307E\u305F\u306F RenoDX \u306E Discord \u304B\u3089\u5165\u624B\u3067\u304D\u307E\u3059\u3002mgpu \u30D5\u30A9\u30EB\u30C0\u30FC\u306B\u7F6E\u3044\u3066\u304F\u3060\u3055\u3044\u3002\u300C\u30A4\u30F3\u30B9\u30C8\u30FC\u30EB\u300D\u30DA\u30FC\u30B8\u306E\u624B\u9806 2 \u3068 3 \u3092\u53C2\u7167\u3057\u3066\u304F\u3060\u3055\u3044\u3002\r\n", L"\r\n\uAC8C\uC784\uC758 mgpu \uD3F4\uB354\uC5D0 nvngx_dlssnr.dll\uC774 \uC5C6\uC2B5\uB2C8\uB2E4. bridge\uC5D0\uB294 \uD3EC\uD568\uB418\uC5B4 \uC788\uC9C0 \uC54A\uC2B5\uB2C8\uB2E4. RHI(github.com/RankFTW/RHI) \uB610\uB294 RenoDX \uB514\uC2A4\uCF54\uB4DC\uC5D0\uC11C \uBC1B\uC744 \uC218 \uC788\uC2B5\uB2C8\uB2E4. mgpu \uD3F4\uB354\uC5D0 \uB123\uC73C\uC138\uC694. \uC124\uCE58 \uD398\uC774\uC9C0 2, 3\uB2E8\uACC4\uB97C \uCC38\uACE0\uD558\uC138\uC694.\r\n" } },
        { L"\r\nOther ReShade add-ons found: ", { L"\r\nOutros add-ons do ReShade encontrados: ", L"\r\nSe encontraron otros add-ons de ReShade: ", L"\r\n\u627E\u5230\u5176\u4ED6 ReShade \u63D2\u4EF6\uFF1A", L"\r\n\u4ED6\u306E ReShade \u30A2\u30C9\u30AA\u30F3\u304C\u898B\u3064\u304B\u308A\u307E\u3057\u305F\uFF1A", L"\r\n\uB2E4\uB978 ReShade \uC560\uB4DC\uC628 \uBC1C\uACAC: " } },
        { L"\r\nMove them out of the game folder. See the INSTALL page, step 5.\r\n", { L"\r\nTire-os da pasta do jogo. Veja a p\u00E1gina INSTALAR, passo 5.\r\n", L"\r\nS\u00E1calos de la carpeta del juego. Mira la p\u00E1gina INSTALAR, paso 5.\r\n", L"\r\n\u8BF7\u5C06\u5B83\u4EEC\u79FB\u51FA\u6E38\u620F\u6587\u4EF6\u5939\u3002\u89C1\u201C\u5B89\u88C5\u201D\u9875\u9762\u7B2C 5 \u6B65\u3002\r\n", L"\r\n\u30B2\u30FC\u30E0\u30D5\u30A9\u30EB\u30C0\u30FC\u306E\u5916\u3078\u79FB\u3057\u3066\u304F\u3060\u3055\u3044\u3002\u300C\u30A4\u30F3\u30B9\u30C8\u30FC\u30EB\u300D\u30DA\u30FC\u30B8\u306E\u624B\u9806 5 \u3092\u53C2\u7167\u3057\u3066\u304F\u3060\u3055\u3044\u3002\r\n", L"\r\n\uAC8C\uC784 \uD3F4\uB354 \uBC16\uC73C\uB85C \uC62E\uAE30\uC138\uC694. \uC124\uCE58 \uD398\uC774\uC9C0 5\uB2E8\uACC4\uB97C \uCC38\uACE0\uD558\uC138\uC694.\r\n" } },
        { L"Copied %d file(s), kept %d existing config file(s), %d problem(s).\r\n%s", { L"%d arquivo(s) copiado(s), %d arquivo(s) de configura\u00E7\u00E3o mantido(s), %d problema(s).\r\n%s", L"Se copiaron %d archivo(s), se conservaron %d archivo(s) de configuraci\u00F3n, %d problema(s).\r\n%s", L"\u5DF2\u590D\u5236 %d \u4E2A\u6587\u4EF6\uFF0C\u4FDD\u7559 %d \u4E2A\u73B0\u6709\u914D\u7F6E\u6587\u4EF6\uFF0C%d \u4E2A\u95EE\u9898\u3002\r\n%s", L"%d \u500B\u306E\u30D5\u30A1\u30A4\u30EB\u3092\u30B3\u30D4\u30FC\u3001\u65E2\u5B58\u306E\u8A2D\u5B9A\u30D5\u30A1\u30A4\u30EB %d \u500B\u3092\u4FDD\u6301\u3001\u554F\u984C %d \u4EF6\u3002\r\n%s", L"\uD30C\uC77C %d\uAC1C \uBCF5\uC0AC, \uAE30\uC874 \uC124\uC815 \uD30C\uC77C %d\uAC1C \uC720\uC9C0, \uBB38\uC81C %d\uAC74.\r\n%s" } },
        { L"Always: the composition mode runs with any number of displays, without checking which card the game window's display is on. If that display is on the card that renders the game, the picture travels back across the bus.\n\nThe default already composes by itself when the game window is on the DLSS 5 card.\n\nSet Always for this game?", { L"Sempre: o modo de composi\u00E7\u00E3o funciona com qualquer n\u00FAmero de monitores, sem verificar em qual placa est\u00E1 o monitor da janela do jogo. Se esse monitor estiver na placa que renderiza o jogo, a imagem volta pelo barramento.\n\nO padr\u00E3o j\u00E1 comp\u00F5e sozinho quando a janela do jogo est\u00E1 na placa DLSS 5.\n\nDefinir Sempre para este jogo?", L"Siempre: el modo de composici\u00F3n funciona con cualquier n\u00FAmero de monitores, sin comprobar en qu\u00E9 tarjeta est\u00E1 el monitor de la ventana del juego. Si ese monitor est\u00E1 en la tarjeta que renderiza el juego, la imagen vuelve por el bus.\n\nEl modo predeterminado ya compone solo cuando la ventana del juego est\u00E1 en la tarjeta DLSS 5.\n\n\u00BFFijar Siempre para este juego?", L"\u59CB\u7EC8\uFF1A\u5408\u6210\u6A21\u5F0F\u5728\u4EFB\u610F\u6570\u91CF\u7684\u663E\u793A\u5668\u4E0B\u8FD0\u884C\uFF0C\u4E0D\u68C0\u67E5\u6E38\u620F\u7A97\u53E3\u6240\u5728\u7684\u663E\u793A\u5668\u8FDE\u63A5\u5728\u54EA\u5F20\u663E\u5361\u4E0A\u3002\u5982\u679C\u8BE5\u663E\u793A\u5668\u8FDE\u63A5\u5728\u6E32\u67D3\u6E38\u620F\u7684\u663E\u5361\u4E0A\uFF0C\u753B\u9762\u4F1A\u7ECF\u603B\u7EBF\u4F20\u56DE\u3002\n\n\u9ED8\u8BA4\u8BBE\u7F6E\u5728\u6E38\u620F\u7A97\u53E3\u4F4D\u4E8E DLSS 5 \u663E\u5361\u4E0A\u65F6\u5DF2\u4F1A\u81EA\u52A8\u5408\u6210\u3002\n\n\u4E3A\u6B64\u6E38\u620F\u8BBE\u4E3A\u201C\u59CB\u7EC8\u201D\uFF1F", L"\u5E38\u306B\uFF1A\u5408\u6210\u30E2\u30FC\u30C9\u306F\u30C7\u30A3\u30B9\u30D7\u30EC\u30A4\u306E\u6570\u306B\u95A2\u4FC2\u306A\u304F\u52D5\u4F5C\u3057\u3001\u30B2\u30FC\u30E0\u30A6\u30A3\u30F3\u30C9\u30A6\u306E\u30C7\u30A3\u30B9\u30D7\u30EC\u30A4\u304C\u3069\u306E\u30AB\u30FC\u30C9\u306B\u63A5\u7D9A\u3055\u308C\u3066\u3044\u308B\u304B\u3092\u78BA\u8A8D\u3057\u307E\u305B\u3093\u3002\u305D\u306E\u30C7\u30A3\u30B9\u30D7\u30EC\u30A4\u304C\u30B2\u30FC\u30E0\u3092\u63CF\u753B\u3059\u308B\u30AB\u30FC\u30C9\u306B\u63A5\u7D9A\u3055\u308C\u3066\u3044\u308B\u5834\u5408\u3001\u753B\u50CF\u306F\u30D0\u30B9\u3092\u901A\u3063\u3066\u623B\u308A\u307E\u3059\u3002\n\n\u65E2\u5B9A\u3067\u306F\u30B2\u30FC\u30E0\u30A6\u30A3\u30F3\u30C9\u30A6\u304CDLSS 5 \u30AB\u30FC\u30C9\u4E0A\u306B\u3042\u308B\u3068\u304D\u81EA\u52D5\u3067\u5408\u6210\u3057\u307E\u3059\u3002\n\n\u3053\u306E\u30B2\u30FC\u30E0\u3092\u300C\u5E38\u306B\u300D\u306B\u8A2D\u5B9A\u3057\u307E\u3059\u304B\uFF1F", L"\uD56D\uC0C1: \uD569\uC131 \uBAA8\uB4DC\uAC00 \uBAA8\uB2C8\uD130 \uC218\uC640 \uAD00\uACC4\uC5C6\uC774 \uB3D9\uC791\uD558\uBA70, \uAC8C\uC784 \uCC3D\uC758 \uBAA8\uB2C8\uD130\uAC00 \uC5B4\uB290 \uCE74\uB4DC\uC5D0 \uC5F0\uACB0\uB418\uC5B4 \uC788\uB294\uC9C0 \uD655\uC778\uD558\uC9C0 \uC54A\uC2B5\uB2C8\uB2E4. \uADF8 \uBAA8\uB2C8\uD130\uAC00 \uAC8C\uC784\uC744 \uB80C\uB354\uB9C1\uD558\uB294 \uCE74\uB4DC\uC5D0 \uC5F0\uACB0\uB418\uC5B4 \uC788\uC73C\uBA74 \uD654\uBA74\uC774 \uBC84\uC2A4\uB97C \uAC70\uCCD0 \uB418\uB3CC\uC544\uAC11\uB2C8\uB2E4.\n\n\uAE30\uBCF8\uAC12\uC740 \uAC8C\uC784 \uCC3D\uC774 DLSS 5 \uCE74\uB4DC\uC5D0 \uC788\uC744 \uB54C \uC774\uBBF8 \uC790\uB3D9\uC73C\uB85C \uD569\uC131\uD569\uB2C8\uB2E4.\n\n\uC774 \uAC8C\uC784\uC744 '\uD56D\uC0C1'\uC73C\uB85C \uC124\uC815\uD560\uAE4C\uC694?" } },
        { L"With LaunchRecord=1 the add-on refreshes mgpu\\last_launch.ini every 300 frames while the game runs, with the frame rate of each card. It is written on the bridge thread with the lock released. If one write takes more than 2 ms the add-on turns it off by itself and the next launch runs without it.\n\nTurn it on for this game?", { L"Com LaunchRecord=1 o add-on atualiza mgpu\\last_launch.ini a cada 300 quadros durante o jogo, com a taxa de quadros de cada placa. A grava\u00E7\u00E3o \u00E9 feita na thread do bridge com o bloqueio liberado. Se uma grava\u00E7\u00E3o levar mais de 2 ms, o add-on desliga sozinho e a pr\u00F3xima execu\u00E7\u00E3o roda sem ele.\n\nLigar para este jogo?", L"Con LaunchRecord=1 el add-on actualiza mgpu\\last_launch.ini cada 300 fotogramas mientras se juega, con la tasa de fotogramas de cada tarjeta. Se escribe en el hilo del bridge con el bloqueo liberado. Si una escritura tarda m\u00E1s de 2 ms, el add-on lo apaga solo y la siguiente ejecuci\u00F3n va sin \u00E9l.\n\n\u00BFActivarlo para este juego?", L"LaunchRecord=1 \u65F6\uFF0C\u63D2\u4EF6\u5728\u6E38\u620F\u8FD0\u884C\u671F\u95F4\u6BCF 300 \u5E27\u5237\u65B0\u4E00\u6B21 mgpu\\last_launch.ini\uFF0C\u8BB0\u5F55\u6BCF\u5F20\u663E\u5361\u7684\u5E27\u7387\u3002\u5199\u5165\u5728 bridge \u7EBF\u7A0B\u4E0A\u8FDB\u884C\uFF0C\u4E0D\u6301\u6709\u9501\u3002\u5982\u679C\u67D0\u6B21\u5199\u5165\u8D85\u8FC7 2 \u6BEB\u79D2\uFF0C\u63D2\u4EF6\u4F1A\u81EA\u52A8\u5173\u95ED\u6B64\u529F\u80FD\uFF0C\u4E0B\u6B21\u542F\u52A8\u65F6\u4E0D\u518D\u4F7F\u7528\u3002\n\n\u4E3A\u6B64\u6E38\u620F\u5F00\u542F\uFF1F", L"LaunchRecord=1 \u3067\u306F\u3001\u30B2\u30FC\u30E0\u5B9F\u884C\u4E2D 300 \u30D5\u30EC\u30FC\u30E0\u3054\u3068\u306B mgpu\\last_launch.ini \u3092\u66F4\u65B0\u3057\u3001\u5404\u30AB\u30FC\u30C9\u306E\u30D5\u30EC\u30FC\u30E0\u30EC\u30FC\u30C8\u3092\u8A18\u9332\u3057\u307E\u3059\u3002\u66F8\u304D\u8FBC\u307F\u306F\u30ED\u30C3\u30AF\u3092\u89E3\u653E\u3057\u305F\u72B6\u614B\u3067 bridge \u30B9\u30EC\u30C3\u30C9\u4E0A\u3067\u884C\u3044\u307E\u3059\u30021 \u56DE\u306E\u66F8\u304D\u8FBC\u307F\u304C 2 ms \u3092\u8D85\u3048\u308B\u3068\u3001\u30A2\u30C9\u30AA\u30F3\u304C\u81EA\u52D5\u3067\u30AA\u30D5\u306B\u3057\u3001\u6B21\u56DE\u306E\u8D77\u52D5\u3067\u306F\u4F7F\u3044\u307E\u305B\u3093\u3002\n\n\u3053\u306E\u30B2\u30FC\u30E0\u3067\u30AA\u30F3\u306B\u3057\u307E\u3059\u304B\uFF1F", L"LaunchRecord=1\uC774\uBA74 \uAC8C\uC784 \uC2E4\uD589 \uC911 300\uD504\uB808\uC784\uB9C8\uB2E4 mgpu\\last_launch.ini\uB97C \uAC31\uC2E0\uD558\uACE0 \uCE74\uB4DC\uBCC4 \uD504\uB808\uC784 \uB808\uC774\uD2B8\uB97C \uAE30\uB85D\uD569\uB2C8\uB2E4. \uC4F0\uAE30\uB294 \uC7A0\uAE08\uC744 \uD47C \uC0C1\uD0DC\uB85C bridge \uC2A4\uB808\uB4DC\uC5D0\uC11C \uD569\uB2C8\uB2E4. \uD55C \uBC88\uC758 \uC4F0\uAE30\uAC00 2ms\uB97C \uB118\uC73C\uBA74 \uC560\uB4DC\uC628\uC774 \uC2A4\uC2A4\uB85C \uB044\uACE0 \uB2E4\uC74C \uC2E4\uD589\uC740 \uC774 \uAE30\uB2A5 \uC5C6\uC774 \uC9C4\uD589\uB429\uB2C8\uB2E4.\n\n\uC774 \uAC8C\uC784\uC5D0\uC11C \uCF24\uAE4C\uC694?" } },
        { L"Are you sure? This option has not been tested.\n\nPlease test fullscreen and borderless first (which one works is game dependent). The add-on already tries to fit the picture by itself when it sees the mismatch (DcompFit=auto); Force fit makes it scale always.\n\nWhat it does: the compositor scales the DLSS 5 picture to the game window instead of showing it 1:1. The DLSS 5 stage still runs at the game's render size. Takes effect on the next launch.", { L"Tem certeza? Esta op\u00E7\u00E3o n\u00E3o foi testada.\n\nTeste antes tela cheia e sem bordas (qual funciona depende do jogo). O add-on j\u00E1 tenta ajustar a imagem sozinho quando v\u00EA a diferen\u00E7a (DcompFit=auto); o ajuste for\u00E7ado faz ele sempre redimensionar.\n\nO que faz: o compositor ajusta a imagem DLSS 5 \u00E0 janela do jogo em vez de mostr\u00E1-la 1:1. O est\u00E1gio DLSS 5 continua na resolu\u00E7\u00E3o de renderiza\u00E7\u00E3o do jogo. Vale a partir da pr\u00F3xima execu\u00E7\u00E3o.", L"\u00BFSeguro? Esta opci\u00F3n no se ha probado.\n\nPrueba antes pantalla completa y sin bordes (cu\u00E1l funciona depende del juego). El add-on ya intenta ajustar la imagen solo cuando ve la diferencia (DcompFit=auto); el ajuste forzado hace que siempre escale.\n\nQu\u00E9 hace: el compositor escala la imagen DLSS 5 a la ventana del juego en vez de mostrarla 1:1. La etapa DLSS 5 sigue a la resoluci\u00F3n de renderizado del juego. Se aplica en la siguiente ejecuci\u00F3n.", L"\u786E\u5B9A\u5417\uFF1F\u6B64\u9009\u9879\u5C1A\u672A\u7ECF\u8FC7\u6D4B\u8BD5\u3002\n\n\u8BF7\u5148\u6D4B\u8BD5\u5168\u5C4F\u548C\u65E0\u8FB9\u6846\uFF08\u54EA\u79CD\u53EF\u7528\u53D6\u51B3\u4E8E\u6E38\u620F\uFF09\u3002\u63D2\u4EF6\u5728\u53D1\u73B0\u4E0D\u4E00\u81F4\u65F6\u5DF2\u4F1A\u81EA\u52A8\u9002\u914D\u753B\u9762\uFF08DcompFit=auto\uFF09\uFF1B\u5F3A\u5236\u9002\u914D\u4F1A\u8BA9\u5B83\u59CB\u7EC8\u7F29\u653E\u3002\n\n\u4F5C\u7528\uFF1A\u5408\u6210\u5668\u5C06 DLSS 5 \u753B\u9762\u7F29\u653E\u5230\u6E38\u620F\u7A97\u53E3\uFF0C\u800C\u4E0D\u662F 1:1 \u663E\u793A\u3002DLSS 5 \u5904\u7406\u4ECD\u4EE5\u6E38\u620F\u7684\u6E32\u67D3\u5206\u8FA8\u7387\u8FD0\u884C\u3002\u4E0B\u6B21\u542F\u52A8\u65F6\u751F\u6548\u3002", L"\u3088\u308D\u3057\u3044\u3067\u3059\u304B\uFF1F\u3053\u306E\u30AA\u30D7\u30B7\u30E7\u30F3\u306F\u672A\u691C\u8A3C\u3067\u3059\u3002\n\n\u307E\u305A\u30D5\u30EB\u30B9\u30AF\u30EA\u30FC\u30F3\u3068\u30DC\u30FC\u30C0\u30FC\u30EC\u30B9\u3092\u8A66\u3057\u3066\u304F\u3060\u3055\u3044\uFF08\u3069\u3061\u3089\u304C\u52D5\u304F\u304B\u306F\u30B2\u30FC\u30E0\u306B\u3088\u308A\u307E\u3059\uFF09\u3002\u30A2\u30C9\u30AA\u30F3\u306F\u4E0D\u4E00\u81F4\u3092\u691C\u51FA\u3059\u308B\u3068\u81EA\u52D5\u3067\u753B\u50CF\u3092\u5408\u308F\u305B\u3088\u3046\u3068\u3057\u307E\u3059\uFF08DcompFit=auto\uFF09\u3002\u5F37\u5236\u30D5\u30A3\u30C3\u30C8\u306F\u5E38\u306B\u62E1\u5927\u7E2E\u5C0F\u3057\u307E\u3059\u3002\n\n\u52D5\u4F5C\uFF1A\u5408\u6210\u51E6\u7406\u304CDLSS 5 \u753B\u50CF\u3092 1:1 \u3067\u306F\u306A\u304F\u30B2\u30FC\u30E0\u30A6\u30A3\u30F3\u30C9\u30A6\u306B\u5408\u308F\u305B\u3066\u62E1\u5927\u7E2E\u5C0F\u3057\u307E\u3059\u3002DLSS 5 \u51E6\u7406\u306F\u30B2\u30FC\u30E0\u306E\u63CF\u753B\u89E3\u50CF\u5EA6\u306E\u307E\u307E\u3067\u3059\u3002\u6B21\u56DE\u306E\u8D77\u52D5\u304B\u3089\u6709\u52B9\u3067\u3059\u3002", L"\uC815\uB9D0\uB85C \uD560\uAE4C\uC694? \uC774 \uC635\uC158\uC740 \uD14C\uC2A4\uD2B8\uB418\uC9C0 \uC54A\uC558\uC2B5\uB2C8\uB2E4.\n\n\uBA3C\uC800 \uC804\uCCB4 \uD654\uBA74\uACFC \uD14C\uB450\uB9AC \uC5C6\uC74C\uC744 \uC2DC\uD5D8\uD558\uC138\uC694 (\uC5B4\uB290 \uCABD\uC774 \uB418\uB294\uC9C0\uB294 \uAC8C\uC784\uB9C8\uB2E4 \uB2E4\uB985\uB2C8\uB2E4). \uC560\uB4DC\uC628\uC740 \uBD88\uC77C\uCE58\uB97C \uAC10\uC9C0\uD558\uBA74 \uC774\uBBF8 \uC2A4\uC2A4\uB85C \uD654\uBA74\uC744 \uB9DE\uCD94\uB824\uACE0 \uD569\uB2C8\uB2E4 (DcompFit=auto). \uAC15\uC81C \uB9DE\uCDA4\uC740 \uD56D\uC0C1 \uD06C\uAE30\uB97C \uC870\uC808\uD569\uB2C8\uB2E4.\n\n\uB3D9\uC791: \uD569\uC131\uAE30\uAC00 DLSS 5 \uD654\uBA74\uC744 1:1\uB85C \uBCF4\uC5EC \uC8FC\uC9C0 \uC54A\uACE0 \uAC8C\uC784 \uCC3D\uC5D0 \uB9DE\uCDB0 \uD06C\uAE30\uB97C \uC870\uC808\uD569\uB2C8\uB2E4. DLSS 5 \uB2E8\uACC4\uB294 \uAC8C\uC784\uC758 \uB80C\uB354\uB9C1 \uD574\uC0C1\uB3C4\uB85C \uACC4\uC18D \uC2E4\uD589\uB429\uB2C8\uB2E4. \uB2E4\uC74C \uC2E4\uD589\uBD80\uD130 \uC801\uC6A9\uB429\uB2C8\uB2E4." } },
        { L"Diagnose cannot tell which card does the DLSS 5 work. The last launch did not record a card, and there is not exactly one NVIDIA card besides the one that drives the main display.\n\nRun the game once with the bridge installed, then press SCAN DISPLAY again.", { L"O Diagn\u00F3stico n\u00E3o consegue saber qual placa faz o trabalho do DLSS 5. A \u00FAltima execu\u00E7\u00E3o n\u00E3o registrou uma placa, e n\u00E3o h\u00E1 exatamente uma placa NVIDIA al\u00E9m da que controla o monitor principal.\n\nExecute o jogo uma vez com o bridge instalado e clique em ESCANEAR MONITOR de novo.", L"Diagn\u00F3stico no puede saber qu\u00E9 tarjeta hace el trabajo de DLSS 5. La \u00FAltima ejecuci\u00F3n no registr\u00F3 una tarjeta, y no hay exactamente una tarjeta NVIDIA adem\u00E1s de la que controla el monitor principal.\n\nEjecuta el juego una vez con el bridge instalado y pulsa ESCANEAR MONITOR otra vez.", L"\u8BCA\u65AD\u65E0\u6CD5\u786E\u5B9A\u54EA\u5F20\u663E\u5361\u8D1F\u8D23 DLSS 5\u3002\u4E0A\u6B21\u542F\u52A8\u6CA1\u6709\u8BB0\u5F55\u663E\u5361\uFF0C\u4E14\u9664\u9A71\u52A8\u4E3B\u663E\u793A\u5668\u7684\u663E\u5361\u5916\uFF0C\u5E76\u975E\u6070\u597D\u53EA\u6709\u4E00\u5F20 NVIDIA \u663E\u5361\u3002\n\n\u8BF7\u5728\u5B89\u88C5 bridge \u540E\u8FD0\u884C\u4E00\u6B21\u6E38\u620F\uFF0C\u7136\u540E\u518D\u6B21\u70B9\u51FB\u201C\u626B\u63CF\u663E\u793A\u5668\u201D\u3002", L"\u8A3A\u65AD\u3067\u306F DLSS 5 \u3092\u51E6\u7406\u3059\u308B\u30AB\u30FC\u30C9\u3092\u5224\u65AD\u3067\u304D\u307E\u305B\u3093\u3002\u524D\u56DE\u306E\u8D77\u52D5\u3067\u30AB\u30FC\u30C9\u304C\u8A18\u9332\u3055\u308C\u3066\u304A\u3089\u305A\u3001\u30E1\u30A4\u30F3\u30C7\u30A3\u30B9\u30D7\u30EC\u30A4\u3092\u99C6\u52D5\u3057\u3066\u3044\u308B\u30AB\u30FC\u30C9\u4EE5\u5916\u306E NVIDIA \u30AB\u30FC\u30C9\u304C\u3061\u3087\u3046\u3069 1 \u679A\u3067\u306F\u3042\u308A\u307E\u305B\u3093\u3002\n\nbridge \u3092\u5165\u308C\u305F\u72B6\u614B\u3067\u30B2\u30FC\u30E0\u3092\u4E00\u5EA6\u8D77\u52D5\u3057\u3066\u304B\u3089\u3001\u3082\u3046\u4E00\u5EA6\u300C\u30C7\u30A3\u30B9\u30D7\u30EC\u30A4\u3092\u30B9\u30AD\u30E3\u30F3\u300D\u3092\u62BC\u3057\u3066\u304F\u3060\u3055\u3044\u3002", L"\uC9C4\uB2E8\uC774 DLSS 5 \uC791\uC5C5\uC744 \uD558\uB294 \uCE74\uB4DC\uB97C \uC54C \uC218 \uC5C6\uC2B5\uB2C8\uB2E4. \uB9C8\uC9C0\uB9C9 \uC2E4\uD589\uC5D0\uC11C \uCE74\uB4DC\uAC00 \uAE30\uB85D\uB418\uC9C0 \uC54A\uC558\uACE0, \uAE30\uBCF8 \uBAA8\uB2C8\uD130\uB97C \uAD6C\uB3D9\uD558\uB294 \uCE74\uB4DC \uC678\uC5D0 NVIDIA \uCE74\uB4DC\uAC00 \uC815\uD655\uD788 \uD55C \uC7A5\uC774 \uC544\uB2D9\uB2C8\uB2E4.\n\nbridge\uB97C \uC124\uCE58\uD55C \uC0C1\uD0DC\uB85C \uAC8C\uC784\uC744 \uD55C \uBC88 \uC2E4\uD589\uD55C \uB4A4 \uBAA8\uB2C8\uD130 \uC2A4\uCE94\uC744 \uB2E4\uC2DC \uB204\uB974\uC138\uC694." } },
        { L"nrcheck.exe and nvngx.dll_nrcheck.dll were not found in the game folder or in the launcher's payload\\tools\\ folder.", { L"nrcheck.exe e nvngx.dll_nrcheck.dll n\u00E3o foram encontrados na pasta do jogo nem na pasta payload\\tools\\ do launcher.", L"No se encontraron nrcheck.exe ni nvngx.dll_nrcheck.dll en la carpeta del juego ni en la carpeta payload\\tools\\ del launcher.", L"\u5728\u6E38\u620F\u6587\u4EF6\u5939\u548C\u542F\u52A8\u5668\u7684 payload\\tools\\ \u6587\u4EF6\u5939\u4E2D\u5747\u672A\u627E\u5230 nrcheck.exe \u548C nvngx.dll_nrcheck.dll\u3002", L"\u30B2\u30FC\u30E0\u30D5\u30A9\u30EB\u30C0\u30FC\u306B\u3082\u30E9\u30F3\u30C1\u30E3\u30FC\u306E payload\\tools\\ \u30D5\u30A9\u30EB\u30C0\u30FC\u306B\u3082 nrcheck.exe \u3068 nvngx.dll_nrcheck.dll \u304C\u898B\u3064\u304B\u308A\u307E\u305B\u3093\u3002", L"\uAC8C\uC784 \uD3F4\uB354\uC640 \uB7F0\uCC98\uC758 payload\\tools\\ \uD3F4\uB354\uC5D0\uC11C nrcheck.exe\uC640 nvngx.dll_nrcheck.dll\uC744 \uCC3E\uC744 \uC218 \uC5C6\uC2B5\uB2C8\uB2E4." } },
        { L"mgpu\\nvngx_dlssnr.dll is not in this game folder. Diagnose needs it (it is the DLSS 5 model). See the INSTALL page, steps 2 and 3.", { L"mgpu\\nvngx_dlssnr.dll n\u00E3o est\u00E1 nesta pasta de jogo. O Diagn\u00F3stico precisa dele (\u00E9 o modelo DLSS 5). Veja a p\u00E1gina INSTALAR, passos 2 e 3.", L"mgpu\\nvngx_dlssnr.dll no est\u00E1 en esta carpeta del juego. Diagn\u00F3stico lo necesita (es el modelo DLSS 5). Mira la p\u00E1gina INSTALAR, pasos 2 y 3.", L"\u6B64\u6E38\u620F\u6587\u4EF6\u5939\u4E2D\u6CA1\u6709 mgpu\\nvngx_dlssnr.dll\u3002\u8BCA\u65AD\u9700\u8981\u5B83\uFF08\u5B83\u662F DLSS 5 \u6A21\u578B\uFF09\u3002\u89C1\u201C\u5B89\u88C5\u201D\u9875\u9762\u7B2C 2\u30013 \u6B65\u3002", L"\u3053\u306E\u30B2\u30FC\u30E0\u30D5\u30A9\u30EB\u30C0\u30FC\u306B mgpu\\nvngx_dlssnr.dll \u304C\u3042\u308A\u307E\u305B\u3093\u3002\u8A3A\u65AD\u306B\u5FC5\u8981\u3067\u3059\uFF08DLSS 5 \u30E2\u30C7\u30EB\u3067\u3059\uFF09\u3002\u300C\u30A4\u30F3\u30B9\u30C8\u30FC\u30EB\u300D\u30DA\u30FC\u30B8\u306E\u624B\u9806 2 \u3068 3 \u3092\u53C2\u7167\u3057\u3066\u304F\u3060\u3055\u3044\u3002", L"\uC774 \uAC8C\uC784 \uD3F4\uB354\uC5D0 mgpu\\nvngx_dlssnr.dll\uC774 \uC5C6\uC2B5\uB2C8\uB2E4. \uC9C4\uB2E8\uC5D0 \uD544\uC694\uD569\uB2C8\uB2E4 (DLSS 5 \uBAA8\uB378\uC785\uB2C8\uB2E4). \uC124\uCE58 \uD398\uC774\uC9C0 2, 3\uB2E8\uACC4\uB97C \uCC38\uACE0\uD558\uC138\uC694." } },
        { L"Diagnose runs DLSS-NR (mgpu\\nvngx_dlssnr.dll) on \"%s\" at %s with %d pass(es), about 10 seconds, no game. Close the game first.\n\nRun it now?", { L"O Diagn\u00F3stico executa o DLSS-NR (mgpu\\nvngx_dlssnr.dll) em \"%s\" a %s com %d passagem(ns), cerca de 10 segundos, sem jogo. Feche o jogo antes.\n\nExecutar agora?", L"Diagn\u00F3stico ejecuta DLSS-NR (mgpu\\nvngx_dlssnr.dll) en \"%s\" a %s con %d pasada(s), unos 10 segundos, sin juego. Cierra el juego antes.\n\n\u00BFEjecutar ahora?", L"\u8BCA\u65AD\u5C06\u5728\u201C%s\u201D\u4E0A\u4EE5 %s\u3001%d \u904D\u8FD0\u884C DLSS-NR\uFF08mgpu\\nvngx_dlssnr.dll\uFF09\uFF0C\u7EA6 10 \u79D2\uFF0C\u65E0\u9700\u6E38\u620F\u3002\u8BF7\u5148\u5173\u95ED\u6E38\u620F\u3002\n\n\u73B0\u5728\u8FD0\u884C\u5417\uFF1F", L"\u8A3A\u65AD\u306F\u300C%s\u300D\u3067 %s\u3001%d \u30D1\u30B9\u306E DLSS-NR\uFF08mgpu\\nvngx_dlssnr.dll\uFF09\u3092\u5B9F\u884C\u3057\u307E\u3059\u3002\u7D04 10 \u79D2\u3001\u30B2\u30FC\u30E0\u306F\u4E0D\u8981\u3067\u3059\u3002\u5148\u306B\u30B2\u30FC\u30E0\u3092\u9589\u3058\u3066\u304F\u3060\u3055\u3044\u3002\n\n\u4ECA\u3059\u3050\u5B9F\u884C\u3057\u307E\u3059\u304B\uFF1F", L"\uC9C4\uB2E8\uC740 \"%s\"\uC5D0\uC11C %s, %d\uD328\uC2A4\uB85C DLSS-NR(mgpu\\nvngx_dlssnr.dll)\uC744 \uC2E4\uD589\uD569\uB2C8\uB2E4. \uC57D 10\uCD08, \uAC8C\uC784 \uC5C6\uC774 \uC9C4\uD589\uD569\uB2C8\uB2E4. \uBA3C\uC800 \uAC8C\uC784\uC744 \uB2EB\uC73C\uC138\uC694.\n\n\uC9C0\uAE08 \uC2E4\uD589\uD560\uAE4C\uC694?" } },
        { L" (the card the bridge chose on the last launch)", { L" (a placa que o bridge escolheu na \u00FAltima execu\u00E7\u00E3o)", L" (la tarjeta que eligi\u00F3 el bridge en la \u00FAltima ejecuci\u00F3n)", L"\uFF08bridge \u4E0A\u6B21\u542F\u52A8\u65F6\u9009\u62E9\u7684\u663E\u5361\uFF09", L"\uFF08\u524D\u56DE\u306E\u8D77\u52D5\u3067 bridge \u304C\u9078\u3093\u3060\u30AB\u30FC\u30C9\uFF09", L" (bridge\uAC00 \uB9C8\uC9C0\uB9C9 \uC2E4\uD589\uC5D0\uC11C \uC120\uD0DD\uD55C \uCE74\uB4DC)" } },
        { L" (the only NVIDIA card that does not drive the main display)", { L" (a \u00FAnica placa NVIDIA que n\u00E3o controla o monitor principal)", L" (la \u00FAnica tarjeta NVIDIA que no controla el monitor principal)", L"\uFF08\u552F\u4E00\u4E00\u5F20\u4E0D\u9A71\u52A8\u4E3B\u663E\u793A\u5668\u7684 NVIDIA \u663E\u5361\uFF09", L"\uFF08\u30E1\u30A4\u30F3\u30C7\u30A3\u30B9\u30D7\u30EC\u30A4\u3092\u99C6\u52D5\u3057\u3066\u3044\u306A\u3044\u552F\u4E00\u306E NVIDIA \u30AB\u30FC\u30C9\uFF09", L" (\uAE30\uBCF8 \uBAA8\uB2C8\uD130\uB97C \uAD6C\uB3D9\uD558\uC9C0 \uC54A\uB294 \uC720\uC77C\uD55C NVIDIA \uCE74\uB4DC)" } },
        { L"Diagnose ended with code %lu. nrbench_log.txt in the game folder says why.", { L"O Diagn\u00F3stico terminou com o c\u00F3digo %lu. O nrbench_log.txt na pasta do jogo explica o motivo.", L"Diagn\u00F3stico termin\u00F3 con el c\u00F3digo %lu. nrbench_log.txt en la carpeta del juego explica por qu\u00E9.", L"\u8BCA\u65AD\u4EE5\u4EE3\u7801 %lu \u7ED3\u675F\u3002\u6E38\u620F\u6587\u4EF6\u5939\u4E2D\u7684 nrbench_log.txt \u8BF4\u660E\u4E86\u539F\u56E0\u3002", L"\u8A3A\u65AD\u306F\u30B3\u30FC\u30C9 %lu \u3067\u7D42\u4E86\u3057\u307E\u3057\u305F\u3002\u7406\u7531\u306F\u30B2\u30FC\u30E0\u30D5\u30A9\u30EB\u30C0\u30FC\u306E nrbench_log.txt \u306B\u3042\u308A\u307E\u3059\u3002", L"\uC9C4\uB2E8\uC774 \uCF54\uB4DC %lu(\uC73C)\uB85C \uB05D\uB0AC\uC2B5\uB2C8\uB2E4. \uC774\uC720\uB294 \uAC8C\uC784 \uD3F4\uB354\uC758 nrbench_log.txt\uC5D0 \uC788\uC2B5\uB2C8\uB2E4." } },
        { L"Pick the game folder (where the game's exe and ReShade are)", { L"Escolha a pasta do jogo (onde est\u00E3o o exe do jogo e o ReShade)", L"Elige la carpeta del juego (donde est\u00E1n el exe del juego y ReShade)", L"\u9009\u62E9\u6E38\u620F\u6587\u4EF6\u5939\uFF08\u6E38\u620F exe \u548C ReShade \u6240\u5728\u4F4D\u7F6E\uFF09", L"\u30B2\u30FC\u30E0\u30D5\u30A9\u30EB\u30C0\u30FC\u3092\u9078\u3093\u3067\u304F\u3060\u3055\u3044\uFF08\u30B2\u30FC\u30E0\u306E exe \u3068 ReShade \u304C\u3042\u308B\u5834\u6240\uFF09", L"\uAC8C\uC784 \uD3F4\uB354\uB97C \uC120\uD0DD\uD558\uC138\uC694 (\uAC8C\uC784 exe\uC640 ReShade\uAC00 \uC788\uB294 \uACF3)" } },
        { L"COPY FILES", { L"COPIAR ARQUIVOS", L"COPIAR ARCHIVOS", L"\u590D\u5236\u6587\u4EF6", L"\u30D5\u30A1\u30A4\u30EB\u3092\u30B3\u30D4\u30FC", L"\uD30C\uC77C \uBCF5\uC0AC" } },
        { L"OPTIONS", { L"OP\u00C7\u00D5ES", L"OPCIONES", L"\u9009\u9879", L"\u30AA\u30D7\u30B7\u30E7\u30F3", L"\uC635\uC158" } },
        { L"MULTI-DISPLAY: ON", { L"MULTIMONITOR: SIM", L"MULTIMONITOR: S\u00CD", L"\u591A\u663E\u793A\u5668\uFF1A\u5F00", L"\u30DE\u30EB\u30C1\u30C7\u30A3\u30B9\u30D7\u30EC\u30A4\uFF1A\u30AA\u30F3", L"\uB2E4\uC911 \uBAA8\uB2C8\uD130: \uCF2C" } },
        { L"MULTI-DISPLAY: OFF", { L"MULTIMONITOR: N\u00C3O", L"MULTIMONITOR: NO", L"\u591A\u663E\u793A\u5668\uFF1A\u5173", L"\u30DE\u30EB\u30C1\u30C7\u30A3\u30B9\u30D7\u30EC\u30A4\uFF1A\u30AA\u30D5", L"\uB2E4\uC911 \uBAA8\uB2C8\uD130: \uB054" } },
        { L"RENAME", { L"RENOMEAR", L"RENOMBRAR", L"\u91CD\u547D\u540D", L"\u540D\u524D\u3092\u5909\u66F4", L"\uC774\uB984 \uBCC0\uACBD" } },
        { L"SAVE", { L"SALVAR", L"GUARDAR", L"\u4FDD\u5B58", L"\u4FDD\u5B58", L"\uC800\uC7A5" } },
        // R329
        { L"OPTIMIZATION", { L"OTIMIZA\u00C7\u00C3O", L"OPTIMIZACI\u00D3N", L"\u4F18\u5316", L"\u6700\u9069\u5316", L"\uCD5C\uC801\uD654" } },
        { L"QUALITY MAX", { L"QUALIDADE M\u00C1XIMA", L"CALIDAD M\u00C1XIMA", L"\u753B\u8D28\u6700\u9AD8", L"\u753B\u8CEA\u6700\u5927", L"\uD654\uC9C8 \uCD5C\uB300" } },
        { L"FPS MAX", { L"FPS M\u00C1XIMO", L"FPS M\u00C1XIMO", L"\u5E27\u7387\u6700\u9AD8", L"FPS \u6700\u5927", L"FPS \uCD5C\uB300" } },
        // R328: dialog captions (the same words as the buttons, in sentence case).
        { L"Copy files", { L"Copiar arquivos", L"Copiar archivos", L"\u590D\u5236\u6587\u4EF6", L"\u30D5\u30A1\u30A4\u30EB\u3092\u30B3\u30D4\u30FC", L"\uD30C\uC77C \uBCF5\uC0AC" } },
        { L"Diagnose", { L"Diagn\u00F3stico", L"Diagn\u00F3stico", L"\u8BCA\u65AD", L"\u8A3A\u65AD", L"\uC9C4\uB2E8" } },
        { L"Force fit", { L"Ajuste for\u00E7ado", L"Ajuste forzado", L"\u5F3A\u5236\u9002\u914D", L"\u5F37\u5236\u30D5\u30A3\u30C3\u30C8", L"\uAC15\uC81C \uB9DE\uCDA4" } },
        { L"Launch record", { L"Registro", L"Registro", L"\u542F\u52A8\u8BB0\u5F55", L"\u8D77\u52D5\u8A18\u9332", L"\uC2E4\uD589 \uAE30\uB85D" } },
        { L"Multi-display", { L"Multimonitor", L"Multimonitor", L"\u591A\u663E\u793A\u5668", L"\u30DE\u30EB\u30C1\u30C7\u30A3\u30B9\u30D7\u30EC\u30A4", L"\uB2E4\uC911 \uBAA8\uB2C8\uD130" } },
        { L"Options", { L"Op\u00E7\u00F5es", L"Opciones", L"\u9009\u9879", L"\u30AA\u30D7\u30B7\u30E7\u30F3", L"\uC635\uC158" } },
        { L"Save", { L"Salvar", L"Guardar", L"\u4FDD\u5B58", L"\u4FDD\u5B58", L"\uC800\uC7A5" } },
        { L"SAVES TO: ", { L"SALVA EM: ", L"SE GUARDA EN: ", L"\u4FDD\u5B58\u5230\uFF1A", L"\u4FDD\u5B58\u5148\uFF1A", L"\uC800\uC7A5 \uC704\uCE58: " } },
        { L"UNSAVED CHANGES", { L"ALTERA\u00C7\u00D5ES N\u00C3O SALVAS", L"CAMBIOS SIN GUARDAR", L"\u6709\u672A\u4FDD\u5B58\u7684\u66F4\u6539", L"\u672A\u4FDD\u5B58\u306E\u5909\u66F4\u3042\u308A", L"\uC800\uC7A5\uB418\uC9C0 \uC54A\uC740 \uBCC0\uACBD" } },
        { L"SAVED", { L"SALVO", L"GUARDADO", L"\u5DF2\u4FDD\u5B58", L"\u4FDD\u5B58\u6E08\u307F", L"\uC800\uC7A5\uB428" } },
        { L"Name this game", { L"Nome do jogo", L"Nombre del juego", L"\u6E38\u620F\u540D\u79F0", L"\u30B2\u30FC\u30E0\u540D", L"\uAC8C\uC784 \uC774\uB984" } },
        { L"The name shown in the library:", { L"O nome mostrado na biblioteca:", L"El nombre que se muestra en la biblioteca:", L"\u5728\u6E38\u620F\u5E93\u4E2D\u663E\u793A\u7684\u540D\u79F0\uFF1A", L"\u30E9\u30A4\u30D6\u30E9\u30EA\u306B\u8868\u793A\u3059\u308B\u540D\u524D\uFF1A", L"\uB77C\uC774\uBE0C\uB7EC\uB9AC\uC5D0 \uD45C\uC2DC\uD560 \uC774\uB984:" } },
        { L"Cancel", { L"Cancelar", L"Cancelar", L"\u53D6\u6D88", L"\u30AD\u30E3\u30F3\u30BB\u30EB", L"\uCDE8\uC18C" } },
        { L"The changes for %s are not saved.\n\nSave them to %s?", { L"As altera\u00E7\u00F5es de %s n\u00E3o foram salvas.\n\nSalvar em %s?", L"Los cambios de %s no se han guardado.\n\n\u00BFGuardarlos en %s?", L"%s \u7684\u66F4\u6539\u5C1A\u672A\u4FDD\u5B58\u3002\n\n\u4FDD\u5B58\u5230 %s \u5417\uFF1F", L"%s \u306E\u5909\u66F4\u306F\u4FDD\u5B58\u3055\u308C\u3066\u3044\u307E\u305B\u3093\u3002\n\n%s \u306B\u4FDD\u5B58\u3057\u307E\u3059\u304B\uFF1F", L"%s\uC758 \uBCC0\uACBD \uC0AC\uD56D\uC774 \uC800\uC7A5\uB418\uC9C0 \uC54A\uC558\uC2B5\uB2C8\uB2E4.\n\n%s\uC5D0 \uC800\uC7A5\uD560\uAE4C\uC694?" } },
        { L"THE ADD-ON DECIDES - MAIN DISPLAY: ", { L"O ADD-ON DECIDE - MONITOR PRINCIPAL: ", L"EL ADD-ON DECIDE - MONITOR PRINCIPAL: ", L"\u7531\u63D2\u4EF6\u51B3\u5B9A - \u4E3B\u663E\u793A\u5668\uFF1A", L"\u30A2\u30C9\u30AA\u30F3\u304C\u6C7A\u5B9A - \u30E1\u30A4\u30F3\u30C7\u30A3\u30B9\u30D7\u30EC\u30A4\uFF1A", L"\uC560\uB4DC\uC628\uC774 \uACB0\uC815 - \uAE30\uBCF8 \uBAA8\uB2C8\uD130: " } },
        { L"THE ADD-ON DECIDES - MAIN DISPLAY NOT FOUND", { L"O ADD-ON DECIDE - SEM MONITOR PRINCIPAL", L"EL ADD-ON DECIDE - SIN MONITOR PRINCIPAL", L"\u7531\u63D2\u4EF6\u51B3\u5B9A - \u672A\u627E\u5230\u4E3B\u663E\u793A\u5668", L"\u30A2\u30C9\u30AA\u30F3\u304C\u6C7A\u5B9A - \u30E1\u30A4\u30F3\u30C7\u30A3\u30B9\u30D7\u30EC\u30A4\u304C\u898B\u3064\u304B\u308A\u307E\u305B\u3093", L"\uC560\uB4DC\uC628\uC774 \uACB0\uC815 - \uAE30\uBCF8 \uBAA8\uB2C8\uD130\uB97C \uCC3E\uC744 \uC218 \uC5C6\uC74C" } },
        { L"OLD DISPLAY= LINE IN MGPU.INI - SAVE REMOVES IT", { L"DISPLAY= ANTIGO NO MGPU.INI - SALVAR REMOVE", L"DISPLAY= ANTIGUO EN MGPU.INI - GUARDAR LO QUITA", L"MGPU.INI \u4E2D\u6709\u65E7\u7684 DISPLAY= \u884C - \u4FDD\u5B58\u65F6\u5220\u9664", L"MGPU.INI \u306B\u53E4\u3044 DISPLAY= \u884C\u3042\u308A - \u4FDD\u5B58\u3067\u524A\u9664", L"MGPU.INI\uC5D0 \uC774\uC804 DISPLAY= \uC904 \uC788\uC74C - \uC800\uC7A5\uD558\uBA74 \uC0AD\uC81C" } },
        // R320
        { L"2ND PASS", { L"2\u00AA PASSAGEM", L"2\u00AA PASADA", L"\u7B2C\u4E8C\u904D", L"\u7B2C2\u30D1\u30B9", L"2\uCC28 \uD328\uC2A4" } },
        { L"MEDIUM", { L"M\u00C9DIO", L"MEDIO", L"\u4E2D", L"\u4E2D", L"\uC911\uAC04" } },
        { L"LIGHT", { L"LEVE", L"LIGERO", L"\u8F7B", L"\u8EFD", L"\uAC00\uBCBC\uC6C0" } },
        { L"FULL", { L"COMPLETO", L"COMPLETO", L"\u5B8C\u6574", L"\u30D5\u30EB", L"\uC804\uCCB4" } },
        { L"PASS 1 SCALE (NRS)", { L"ESCALA DA 1\u00AA PASSAGEM (NRS)", L"ESCALA DE LA 1\u00AA PASADA (NRS)", L"\u7B2C\u4E00\u904D\u7F29\u653E (NRS)", L"\u7B2C1\u30D1\u30B9\u306E\u30B9\u30B1\u30FC\u30EB (NRS)", L"1\uCC28 \uD328\uC2A4 \uBC30\uC728 (NRS)" } },
        { L"This game already has an mgpu.ini. Replace it with the one in this release? Your Dcomp settings are kept.\n\nYes: replace it and keep a backup of the old file.\nNo: replace it with no backup.\nCancel: keep the current mgpu.ini.", { L"Este jogo j\u00E1 tem um mgpu.ini. Substituir pelo desta vers\u00E3o? Suas configura\u00E7\u00F5es Dcomp s\u00E3o mantidas.\n\nSim: substituir e guardar uma c\u00F3pia do arquivo antigo.\nN\u00E3o: substituir sem c\u00F3pia.\nCancelar: manter o mgpu.ini atual.", L"Este juego ya tiene un mgpu.ini. \u00BFReemplazarlo por el de esta versi\u00F3n? Tus ajustes Dcomp se conservan.\n\nS\u00ED: reemplazarlo y guardar una copia del archivo anterior.\nNo: reemplazarlo sin copia.\nCancelar: mantener el mgpu.ini actual.", L"\u6B64\u6E38\u620F\u5DF2\u6709 mgpu.ini\u3002\u8981\u7528\u672C\u7248\u672C\u7684\u6587\u4EF6\u66FF\u6362\u5417\uFF1F\u60A8\u7684 Dcomp \u8BBE\u7F6E\u4F1A\u4FDD\u7559\u3002\n\n\u662F\uFF1A\u66FF\u6362\u5E76\u5907\u4EFD\u65E7\u6587\u4EF6\u3002\n\u5426\uFF1A\u66FF\u6362\uFF0C\u4E0D\u5907\u4EFD\u3002\n\u53D6\u6D88\uFF1A\u4FDD\u7559\u5F53\u524D\u7684 mgpu.ini\u3002", L"\u3053\u306E\u30B2\u30FC\u30E0\u306B\u306F\u3059\u3067\u306B mgpu.ini \u304C\u3042\u308A\u307E\u3059\u3002\u3053\u306E\u30EA\u30EA\u30FC\u30B9\u306E\u3082\u306E\u306B\u7F6E\u304D\u63DB\u3048\u307E\u3059\u304B\uFF1FDcomp \u306E\u8A2D\u5B9A\u306F\u4FDD\u6301\u3055\u308C\u307E\u3059\u3002\n\n\u306F\u3044\uFF1A\u7F6E\u304D\u63DB\u3048\u3066\u53E4\u3044\u30D5\u30A1\u30A4\u30EB\u3092\u30D0\u30C3\u30AF\u30A2\u30C3\u30D7\u3002\n\u3044\u3044\u3048\uFF1A\u30D0\u30C3\u30AF\u30A2\u30C3\u30D7\u306A\u3057\u3067\u7F6E\u304D\u63DB\u3048\u3002\n\u30AD\u30E3\u30F3\u30BB\u30EB\uFF1A\u73FE\u5728\u306E mgpu.ini \u3092\u305D\u306E\u307E\u307E\u4F7F\u3046\u3002", L"\uC774 \uAC8C\uC784\uC5D0\uB294 \uC774\uBBF8 mgpu.ini\uAC00 \uC788\uC2B5\uB2C8\uB2E4. \uC774 \uBC84\uC804\uC758 \uD30C\uC77C\uB85C \uBC14\uAFB8\uC2DC\uACA0\uC2B5\uB2C8\uAE4C? Dcomp \uC124\uC815\uC740 \uC720\uC9C0\uB429\uB2C8\uB2E4.\n\n\uC608: \uBC14\uAFB8\uACE0 \uC774\uC804 \uD30C\uC77C\uC744 \uBC31\uC5C5\uD569\uB2C8\uB2E4.\n\uC544\uB2C8\uC694: \uBC31\uC5C5 \uC5C6\uC774 \uBC14\uAFC9\uB2C8\uB2E4.\n\uCDE8\uC18C: \uD604\uC7AC mgpu.ini\uB97C \uADF8\uB300\uB85C \uB461\uB2C8\uB2E4." } },
        { L"Backup of the old mgpu.ini: ", { L"C\u00F3pia do mgpu.ini antigo: ", L"Copia del mgpu.ini anterior: ", L"\u65E7 mgpu.ini \u7684\u5907\u4EFD\uFF1A", L"\u53E4\u3044 mgpu.ini \u306E\u30D0\u30C3\u30AF\u30A2\u30C3\u30D7\uFF1A", L"\uC774\uC804 mgpu.ini \uBC31\uC5C5: " } },
        { L"The backup could not be written - mgpu.ini was not replaced.", { L"N\u00E3o foi poss\u00EDvel gravar a c\u00F3pia - o mgpu.ini n\u00E3o foi substitu\u00EDdo.", L"No se pudo escribir la copia - mgpu.ini no se reemplaz\u00F3.", L"\u65E0\u6CD5\u5199\u5165\u5907\u4EFD - mgpu.ini \u672A\u88AB\u66FF\u6362\u3002", L"\u30D0\u30C3\u30AF\u30A2\u30C3\u30D7\u3092\u66F8\u304D\u8FBC\u3081\u307E\u305B\u3093\u3067\u3057\u305F - mgpu.ini \u306F\u7F6E\u304D\u63DB\u3048\u3089\u308C\u3066\u3044\u307E\u305B\u3093\u3002", L"\uBC31\uC5C5\uC744 \uC4F8 \uC218 \uC5C6\uC5B4 mgpu.ini\uB97C \uBC14\uAFB8\uC9C0 \uC54A\uC558\uC2B5\uB2C8\uB2E4." } },
        { L"mgpu.ini replaced with the release's.", { L"mgpu.ini substitu\u00EDdo pelo desta vers\u00E3o.", L"mgpu.ini reemplazado por el de esta versi\u00F3n.", L"mgpu.ini \u5DF2\u66FF\u6362\u4E3A\u672C\u7248\u672C\u7684\u6587\u4EF6\u3002", L"mgpu.ini \u3092\u3053\u306E\u30EA\u30EA\u30FC\u30B9\u306E\u3082\u306E\u306B\u7F6E\u304D\u63DB\u3048\u307E\u3057\u305F\u3002", L"mgpu.ini\uB97C \uC774 \uBC84\uC804\uC758 \uD30C\uC77C\uB85C \uBC14\uAFE8\uC2B5\uB2C8\uB2E4." } },
        { L"Kept: ", { L"Mantido: ", L"Conservado: ", L"\u4FDD\u7559\uFF1A", L"\u4FDD\u6301\uFF1A", L"\uC720\uC9C0: " } },
        // R356: the pass settings
        { L"PASS 1 NETWORK AT X2", { L"REDE DA 1\u00AA PASSAGEM EM X2", L"RED DE LA 1\u00AA PASADA EN X2", L"x2 \u65F6\u7684\u7B2C\u4E00\u904D\u7F51\u7EDC", L"x2 \u6642\u306E\u7B2C1\u30D1\u30B9\u306E\u30CD\u30C3\u30C8\u30EF\u30FC\u30AF", L"x2 \uC2DC 1\uCC28 \uD328\uC2A4 \uB124\uD2B8\uC6CC\uD06C" } },
        { L"DLSS ON GPU 1 (OLD)", { L"DLSS NA GPU 1 (ANTIGO)", L"DLSS EN LA GPU 1 (ANTIGUO)", L"GPU 1 \u4E0A\u7684 DLSS\uFF08\u65E7\uFF09", L"GPU 1 \u306E DLSS\uFF08\u65E7\uFF09", L"GPU 1\uC758 DLSS (\uC774\uC804)" } },
        { L"PASS 1 NETWORK", { L"REDE DA 1\u00AA PASSAGEM", L"RED DE LA 1\u00AA PASADA", L"\u7B2C\u4E00\u904D\u7F51\u7EDC", L"\u7B2C1\u30D1\u30B9\u306E\u30CD\u30C3\u30C8\u30EF\u30FC\u30AF", L"1\uCC28 \uD328\uC2A4 \uB124\uD2B8\uC6CC\uD06C" } },
        { L"PASS 2 NETWORK", { L"REDE DA 2\u00AA PASSAGEM", L"RED DE LA 2\u00AA PASADA", L"\u7B2C\u4E8C\u904D\u7F51\u7EDC", L"\u7B2C2\u30D1\u30B9\u306E\u30CD\u30C3\u30C8\u30EF\u30FC\u30AF", L"2\uCC28 \uD328\uC2A4 \uB124\uD2B8\uC6CC\uD06C" } },
        { L"PASS 1 RESOLUTION", { L"RESOLU\u00C7\u00C3O DA 1\u00AA PASSAGEM", L"RESOLUCI\u00D3N DE LA 1\u00AA PASADA", L"\u7B2C\u4E00\u904D\u5206\u8FA8\u7387", L"\u7B2C1\u30D1\u30B9\u306E\u89E3\u50CF\u5EA6", L"1\uCC28 \uD328\uC2A4 \uD574\uC0C1\uB3C4" } },
        { L"PASS 2 RESOLUTION", { L"RESOLU\u00C7\u00C3O DA 2\u00AA PASSAGEM", L"RESOLUCI\u00D3N DE LA 2\u00AA PASADA", L"\u7B2C\u4E8C\u904D\u5206\u8FA8\u7387", L"\u7B2C2\u30D1\u30B9\u306E\u89E3\u50CF\u5EA6", L"2\uCC28 \uD328\uC2A4 \uD574\uC0C1\uB3C4" } },
        { L"DLSS 5 CARD FRAME GEN", { L"GERA\u00C7\u00C3O DE QUADROS (PLACA DLSS 5)", L"GENERACI\u00D3N DE FOTOGRAMAS (TARJETA DLSS 5)", L"DLSS 5 \u663E\u5361\u5E27\u751F\u6210", L"DLSS 5 \u30AB\u30FC\u30C9\u3067\u30D5\u30EC\u30FC\u30E0\u751F\u6210", L"DLSS 5 \uCE74\uB4DC \uD504\uB808\uC784 \uC0DD\uC131" } },
        { L"DEFAULTS", { L"PADR\u00C3O", L"PREDETERMINADO", L"\u9ED8\u8BA4", L"\u65E2\u5B9A", L"\uAE30\uBCF8\uAC12" } },
        { L"RESIDUAL NEURAL FPS+", { L"NEURAL RESIDUAL FPS+", L"NEURAL RESIDUAL FPS+", L"\u6B8B\u5DEE\u795E\u7ECF FPS+", L"\u6B8B\u5DEE\u30CB\u30E5\u30FC\u30E9\u30EB FPS+", L"\uC794\uCC28 \uB274\uB7F4 FPS+" } },
        { L"NATIVE FPS-", { L"NATIVA FPS-", L"NATIVA FPS-", L"\u539F\u751F FPS-", L"\u30CD\u30A4\u30C6\u30A3\u30D6 FPS-", L"\uB124\uC774\uD2F0\uBE0C FPS-" } },
        { L"BEST QUALITY-PERFORMANCE", { L"MELHOR QUALIDADE-DESEMPENHO", L"MEJOR CALIDAD-RENDIMIENTO", L"\u6700\u4F73\u753B\u8D28-\u6027\u80FD", L"\u6700\u9AD8\u753B\u8CEA-\u30D1\u30D5\u30A9\u30FC\u30DE\u30F3\u30B9", L"\uCD5C\uACE0 \uD654\uC9C8-\uC131\uB2A5" } },
        { L"FULL-FP8 ON THE LAST PASS REMOVES THE STROBING WHEN STRUCTURE IS HIGH.", { L"FULL-FP8 NA \u00DALTIMA PASSAGEM REMOVE A CINTILA\u00C7\u00C3O COM ESTRUTURA ALTA.", L"FULL-FP8 EN LA \u00DALTIMA PASADA ELIMINA EL PARPADEO CON ESTRUCTURA ALTA.", L"\u6700\u540E\u4E00\u904D\u4F7F\u7528 FULL-FP8\uFF0C\u53EF\u4EE5\u6D88\u9664\u7ED3\u6784\u8C03\u9AD8\u65F6\u7684\u95EA\u70C1\u3002", L"\u6700\u5F8C\u306E\u30D1\u30B9\u3092 FULL-FP8 \u306B\u3059\u308B\u3068\u3001\u69CB\u9020\u304C\u9AD8\u3044\u3068\u304D\u306E\u3061\u3089\u3064\u304D\u304C\u6D88\u3048\u307E\u3059\u3002", L"\uB9C8\uC9C0\uB9C9 \uD328\uC2A4\uB97C FULL-FP8\uB85C \uD558\uBA74 \uAD6C\uC870 \uAC12\uC774 \uB192\uC744 \uB54C\uC758 \uAE5C\uBE61\uC784\uC774 \uC0AC\uB77C\uC9D1\uB2C8\uB2E4." } },
        { L" (2ND PASS ", { L" (2\u00AA PASSAGEM ", L" (2\u00AA PASADA ", L" (\u7B2C\u4E8C\u904D ", L" (\u7B2C2\u30D1\u30B9 ", L" (2\uCC28 \uD328\uC2A4 " } },
    };
    const wchar_t *tr_lookup(const wchar_t *s)
    {
        if (g_lang == LANG_EN || s == nullptr || *s == 0) return nullptr;
        for (const auto &r : k_tr) if (_wcsicmp(r.en, s) == 0) return r.t[g_lang - 1];
        return nullptr;
    }
    std::wstring tr(const wchar_t *en) { const wchar_t *x = tr_lookup(en); return x ? std::wstring(x) : std::wstring(en ? en : L""); }
    std::wstring tr_draw(const std::wstring &s) { const wchar_t *x = tr_lookup(s.c_str()); return x ? std::wstring(x) : s; }
    int tr_box(HWND h, const wchar_t *text, const wchar_t *cap, UINT flags)
    {
        return MessageBoxW(h, tr(text).c_str(), tr(cap).c_str(), flags);
    }
    const wchar_t *lang_face(int l)
    {
        switch (l)
        {
        case LANG_ZH: return L"Microsoft YaHei UI";
        case LANG_JA: return L"Yu Gothic UI";
        case LANG_KO: return L"Malgun Gothic";
        default:      return L"Segoe UI";
        }
    }
    HFONT lang_font(int px)
    {
        static HFONT cache[LANG_N][6] = {};
        const int p = px < 1 ? 1 : (px > 5 ? 5 : px);
        HFONT &f = cache[g_lang][p];
        if (f == nullptr)
            f = CreateFontW(-(8 * p), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, lang_face(g_lang));
        return f;
    }
    HDC measure_dc() { static HDC d = CreateCompatibleDC(nullptr); return d; }
    int gdi_text_w(const std::wstring &s, int px)
    {
        if (s.empty()) return 0;
        HDC d = measure_dc();
        HGDIOBJ o = SelectObject(d, lang_font(px));
        SIZE z{};
        GetTextExtentPoint32W(d, s.c_str(), (int)s.size(), &z);
        SelectObject(d, o);
        return (int)z.cx;
    }
    void gdi_text(HDC dc, int x, int y, const std::wstring &s, int px, COLORREF c)
    {
        if (s.empty()) return;
        HGDIOBJ o = SelectObject(dc, lang_font(px));
        const int bk = SetBkMode(dc, TRANSPARENT);
        const COLORREF oc = SetTextColor(dc, c);
        TextOutW(dc, x, y - px, s.c_str(), (int)s.size());
        SetTextColor(dc, oc);
        SetBkMode(dc, bk);
        SelectObject(dc, o);
    }

    // ---------------- drawing text ----------------
    //
    // One Windows font for every language (lang_font). shown() is what is
    // drawn for a string: its translation, or in English the string in upper
    // case, as the 5x7 font drew it. Measuring and clipping use what is shown.
    // `gap` is kept for callers written for the 5x7 font; it is not used.
    std::wstring upper(std::wstring s);   // below
    std::wstring shown(const std::wstring &s) { return (g_lang == LANG_EN) ? upper(s) : tr_draw(s); }

    void fill(HDC dc, int x, int y, int w, int h, COLORREF c)
    {
        if (w <= 0 || h <= 0) return;
        RECT r{ x, y, x + w, y + h };
        SetBkColor(dc, c);
        ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &r, L"", 0, nullptr);
    }
    void frame(HDC dc, int x, int y, int w, int h, int t, COLORREF c)
    {
        fill(dc, x, y, w, t, c); fill(dc, x, y + h - t, w, t, c);
        fill(dc, x, y, t, h, c); fill(dc, x + w - t, y, t, h, c);
    }
    int text_w(const std::wstring &s, int px, int gap = -1)
    {
        (void)gap;
        return gdi_text_w(shown(s), px);
    }
    void text(HDC dc, int x, int y, const std::wstring &s, int px, COLORREF c, int gap = -1)
    {
        (void)gap;
        gdi_text(dc, x, y, shown(s), px, c);
    }
    // how many characters of s (as given) fit in maxw
    int fit_count(const std::wstring &s, int px, int maxw)
    {
        if (s.empty() || maxw <= 0) return 0;
        HDC d = measure_dc();
        HGDIOBJ o = SelectObject(d, lang_font(px));
        int n = 0; SIZE z{};
        if (!GetTextExtentExPointW(d, s.c_str(), (int)s.size(), maxw, &n, nullptr, &z)) n = 0;
        SelectObject(d, o);
        return n;
    }
    // a shown string clipped to maxw: the tail is cut and ".." appended
    std::wstring clip(const std::wstring &s, int px, int maxw)
    {
        if (gdi_text_w(s, px) <= maxw) return s;
        const int n = fit_count(s, px, maxw - gdi_text_w(L"..", px));
        return s.substr(0, (size_t)(n > 0 ? n : 0)) + L"..";
    }
    std::wstring fit(const std::wstring &s0, int px, int maxw) { return clip(shown(s0), px, maxw); }
    void textf(HDC dc, int x, int y, int maxw, const std::wstring &s, int px, COLORREF c)
    {
        gdi_text(dc, x, y, fit(s, px, maxw), px, c);
    }
    // centred and clipped to maxw (every centred text is clipped since 0.3.0)
    void text_cf(HDC dc, int cx, int y, int maxw, const std::wstring &s, int px, COLORREF c)
    {
        const std::wstring t = fit(s, px, maxw);
        gdi_text(dc, cx - gdi_text_w(t, px) / 2, y, t, px, c);
    }
    // A row value that may take two lines: the first ends at w1 (the row's
    // buttons), the second - the row below, which has none - is w2 wide. Cut
    // at the last space that fits; text with no space (Chinese, Japanese) is
    // cut at the last character that fits.
    void text_2(HDC dc, int x, int y, int w1, int w2, const std::wstring &s0, COLORREF c)
    {
        const std::wstring s = shown(s0);
        if (gdi_text_w(s, 2) <= w1) { gdi_text(dc, x, y, s, 2, c); return; }
        size_t cut = (size_t)fit_count(s, 2, w1);
        if (cut < s.size() && s[cut] != L' ')
        {
            const size_t sp = (cut > 0) ? s.rfind(L' ', cut - 1) : std::wstring::npos;
            if (sp != std::wstring::npos && sp > 0) cut = sp;
        }
        std::wstring head = s.substr(0, cut), tail = s.substr(cut);
        while (!head.empty() && head.back() == L' ') head.pop_back();
        while (!tail.empty() && tail.front() == L' ') tail.erase(0, 1);
        gdi_text(dc, x, y, head, 2, c);
        gdi_text(dc, x, y + ROW_H, clip(tail, 2, w2), 2, c);
    }
    // a label column: minw, or wider when this language's longest label needs it
    int label_col(std::initializer_list<const wchar_t *> labels, int minw)
    {
        int w = minw;
        for (const wchar_t *s : labels) { const int t = text_w(s, 2) + 16; if (t > w) w = t; }
        return w;
    }
    // the y to give text()/gdi_text so the letters are centred on cy: the cell
    // is drawn at y - px and the letters sit between its internal leading and
    // its baseline (the 5x7 font was centred by its 7 rows; a Windows font
    // centred the same way sat low in the buttons)
    int text_y_centred(int cy, int px)
    {
        HDC d = measure_dc();
        HGDIOBJ o = SelectObject(d, lang_font(px));
        TEXTMETRICW tm{}; GetTextMetricsW(d, &tm);
        SelectObject(d, o);
        return cy - (tm.tmInternalLeading + tm.tmAscent) / 2 + px;
    }
    void section(HDC dc, int x, int y, int w, const std::wstring &label)
    {
        textf(dc, x, y, w, label, 2, C_DIM);
        fill(dc, x, y + 20, w, 1, C_RULE);
    }

    // ---------------- small helpers ----------------
    std::wstring dirname(const std::wstring &p)
    {
        const size_t s = p.find_last_of(L"\\/");
        return (s == std::wstring::npos) ? L"" : p.substr(0, s + 1);
    }
    std::wstring leafname(const std::wstring &p)
    {
        std::wstring q = p;
        while (!q.empty() && (q.back() == L'\\' || q.back() == L'/')) q.pop_back();
        const size_t s = q.find_last_of(L"\\/");
        return (s == std::wstring::npos) ? q : q.substr(s + 1);
    }
    bool file_exists(const std::wstring &p)
    {
        const DWORD a = GetFileAttributesW(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }
    bool dir_exists(const std::wstring &p)
    {
        const DWORD a = GetFileAttributesW(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    }
    std::wstring join(const std::wstring &a, const wchar_t *b)
    {
        std::wstring r = a;
        if (!r.empty() && r.back() != L'\\') r += L'\\';
        r += b;
        return r;
    }
    std::wstring utf8_to_w(const std::string &s)
    {
        if (s.empty()) return L"";
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring w((size_t)n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
        return w;
    }
    std::string w_to_utf8(const std::wstring &w)
    {
        if (w.empty()) return "";
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s((size_t)n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
        return s;
    }
    std::string read_file(const std::wstring &p)
    {
        FILE *f = _wfopen(p.c_str(), L"rb");
        if (f == nullptr) return "";
        std::string s;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
        fclose(f);
        return s;
    }
    bool write_file(const std::wstring &p, const std::string &s)
    {
        const std::wstring tmp = p + L".tmp";
        FILE *f = _wfopen(tmp.c_str(), L"wb");
        if (f == nullptr) return false;
        fwrite(s.data(), 1, s.size(), f);
        fclose(f);
        return MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
    }
    std::string ini_get(const std::string &text, const char *key)
    {
        const std::string k = std::string(key) + "=";
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t eol = text.find('\n', pos);
            if (eol == std::string::npos) eol = text.size();
            std::string ln = text.substr(pos, eol - pos);
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            if (ln.compare(0, k.size(), k) == 0) return ln.substr(k.size());
            pos = eol + 1;
        }
        return "";
    }
    // set or append "key=value", line-anchored, the file's own line ending kept,
    // later duplicates of the key dropped (the add-on's own writer does the same).
    std::string ini_set(const std::string &text, const char *key, const std::string &value)
    {
        const std::string nl = (text.find("\r\n") != std::string::npos) ? "\r\n" : "\n";
        const std::string k = std::string(key) + "=";
        std::string out;
        bool done = false;
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t eol = text.find('\n', pos);
            const bool last = (eol == std::string::npos);
            if (last) eol = text.size();
            std::string ln = text.substr(pos, eol - pos);
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            if (ln.compare(0, k.size(), k) == 0)
            {
                if (!done) { out += k + value + nl; done = true; }
            }
            else
            {
                out += ln;
                if (!last || text.back() == '\n') out += nl;
            }
            pos = eol + 1;
        }
        if (!done)
        {
            if (!out.empty() && out.back() != '\n') out += nl;
            out += k + value + nl;
        }
        return out;
    }
    // drop every "key=" line (0.3.0: a key the person turns back off is removed,
    // so the add-on's own default applies - nothing is written in its place)
    std::string ini_remove(const std::string &text, const char *key)
    {
        const std::string k = std::string(key) + "=";
        std::string out;
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t eol = text.find('\n', pos);
            const size_t end = (eol == std::string::npos) ? text.size() : eol + 1;
            std::string ln = text.substr(pos, end - pos);
            std::string bare = ln;
            while (!bare.empty() && (bare.back() == '\n' || bare.back() == '\r')) bare.pop_back();
            if (bare.compare(0, k.size(), k) != 0) out += ln;
            pos = end;
        }
        return out;
    }
    bool parse_luid(const std::string &s, LUID &out)
    {
        unsigned hi = 0, lo = 0;
        if (s.empty() || sscanf(s.c_str(), "0x%x-0x%x", &hi, &lo) != 2) return false;
        out.HighPart = (LONG)hi; out.LowPart = lo;
        return true;
    }
    bool same_luid(const LUID &a, const LUID &b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }
    std::wstring upper(std::wstring s) { for (auto &c : s) if (c >= L'a' && c <= L'z') c = (wchar_t)(c - L'a' + L'A'); return s; }
    std::wstring fmt1(const std::wstring &s)   // "13.564" -> "13.6"
    {
        wchar_t b[32]; swprintf_s(b, L"%.1f", _wtof(s.c_str()));
        return b;
    }
    std::wstring fmt0(const std::wstring &s)   // "112.4" -> "112"
    {
        wchar_t b[32]; swprintf_s(b, L"%.0f", _wtof(s.c_str()));
        return b;
    }

    // ---------------- our config ----------------
    void load_config()
    {
        g_library.clear(); g_names.clear();
        const std::string t = read_file(g_cfg_path);
        for (int i = 1; i < 200; ++i)
        {
            char k[32]; snprintf(k, sizeof k, "Game%d", i);
            const std::string v = ini_get(t, k);
            if (v.empty()) break;
            g_library.push_back(utf8_to_w(v));
            char kn[32]; snprintf(kn, sizeof kn, "Name%d", i);
            g_names.push_back(utf8_to_w(ini_get(t, kn)));
        }
        g_global_display = utf8_to_w(ini_get(t, "Display"));
        g_global_display_name = utf8_to_w(ini_get(t, "DisplayName"));
        const std::string lc = ini_get(t, "Language");
        for (int i = 0; i < LANG_N; ++i) if (lc == k_lang_code[i]) g_lang = i;
    }
    void save_config()
    {
        CreateDirectoryW(g_cfg_dir.c_str(), nullptr);
        std::string t = "; MGPU Bridge Launcher - the library and the global display choice.\r\n";
        for (size_t i = 0; i < g_library.size(); ++i)
        {
            char k[32]; snprintf(k, sizeof k, "Game%u=", (unsigned)i + 1u);
            t += k; t += w_to_utf8(g_library[i]); t += "\r\n";
            if (i < g_names.size() && !g_names[i].empty())
            {
                char kn[32]; snprintf(kn, sizeof kn, "Name%u=", (unsigned)i + 1u);
                t += kn; t += w_to_utf8(g_names[i]); t += "\r\n";
            }
        }
        t += "Display=" + w_to_utf8(g_global_display) + "\r\n";
        t += "DisplayName=" + w_to_utf8(g_global_display_name) + "\r\n";
        t += std::string("Language=") + k_lang_code[g_lang] + "\r\n";
        write_file(g_cfg_path, t);
    }

    // ---------------- adapters and displays ----------------
    struct adapter_info { LUID luid{}; std::wstring name; UINT vendor = 0; };
    struct display_info
    {
        std::wstring friendly, gdi;
        bool primary = false;
        LUID adapter{};
        bool adapter_ok = false;
        int adapter_idx = -1;        // index into g_adapters, -1 if not in the DXGI table
        unsigned mode_w = 0, mode_h = 0;   // the display's current mode (source mode in the CCD path)
        std::wstring vendor;         // NVIDIA / Intel / AMD / other, from the CCD adapter path
    };
    std::vector<adapter_info> g_adapters;
    std::vector<display_info> g_displays;

    typedef HRESULT (WINAPI *pfn_create_factory1)(REFIID, void **);
    HRESULT create_dxgi_factory1(IDXGIFactory1 **out)
    {
        static pfn_create_factory1 p = nullptr;
        if (p == nullptr)
        {
            wchar_t sys[MAX_PATH] = {};
            GetSystemDirectoryW(sys, MAX_PATH);
            std::wstring path = std::wstring(sys) + L"\\dxgi.dll";
            HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (m != nullptr) p = (pfn_create_factory1)(void *)GetProcAddress(m, "CreateDXGIFactory1");
        }
        if (p == nullptr) return E_FAIL;
        return p(__uuidof(IDXGIFactory1), (void **)out);
    }
    std::vector<adapter_info> enum_adapters()
    {
        std::vector<adapter_info> v;
        IDXGIFactory1 *f = nullptr;
        if (FAILED(create_dxgi_factory1(&f))) return v;
        for (UINT i = 0; ; ++i)
        {
            IDXGIAdapter1 *a = nullptr;
            if (f->EnumAdapters1(i, &a) != S_OK) break;
            DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
            adapter_info ai; ai.luid = d.AdapterLuid; ai.name = d.Description; ai.vendor = d.VendorId;
            if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.VendorId != 0x1414) v.push_back(ai);
            a->Release();
        }
        f->Release();
        return v;
    }
    int adapter_index_of(const LUID &l)
    {
        for (size_t i = 0; i < g_adapters.size(); ++i) if (same_luid(g_adapters[i].luid, l)) return (int)i;
        return -1;
    }
    std::wstring short_card(const std::wstring &n)
    {
        // "NVIDIA GeForce RTX 5060 Ti" -> "RTX 5060 TI": the vendor and the
        // family are not the fact, the card is. Other names are kept whole.
        std::wstring s = n;
        for (const wchar_t *pre : { L"NVIDIA GeForce ", L"NVIDIA ", L"AMD Radeon ", L"AMD ", L"Intel(R) ", L"Intel " })
            if (s.compare(0, wcslen(pre), pre) == 0) { s = s.substr(wcslen(pre)); break; }
        return upper(s);
    }
    bool is_igpu(const adapter_info &a)
    {
        if (a.vendor == 0x8086) return true;   // Intel: the only Intel in a bridge rig is the iGPU
        return a.vendor == 0x1002 && upper(a.name).find(L"GRAPHICS") != std::wstring::npos;   // "AMD Radeon(TM) Graphics"
    }

    std::vector<display_info> enum_displays()
    {
        std::vector<display_info> out;
        typedef LONG (WINAPI *pfn_sizes)(UINT32, UINT32 *, UINT32 *);
        typedef LONG (WINAPI *pfn_query)(UINT32, UINT32 *, DISPLAYCONFIG_PATH_INFO *, UINT32 *, DISPLAYCONFIG_MODE_INFO *, DISPLAYCONFIG_TOPOLOGY_ID *);
        typedef LONG (WINAPI *pfn_devinfo)(DISPLAYCONFIG_DEVICE_INFO_HEADER *);
        struct kmt_open { WCHAR DeviceName[32]; UINT hAdapter; LUID AdapterLuid; UINT VidPnSourceId; };
        struct kmt_close { UINT hAdapter; };
        typedef LONG (WINAPI *pfn_kmt_open)(kmt_open *);
        typedef LONG (WINAPI *pfn_kmt_close)(const kmt_close *);
        HMODULE u = GetModuleHandleW(L"user32.dll"), g = GetModuleHandleW(L"gdi32.dll");
        if (!g) g = LoadLibraryW(L"gdi32.dll");
        pfn_sizes p_sizes = u ? (pfn_sizes)(void *)GetProcAddress(u, "GetDisplayConfigBufferSizes") : nullptr;
        pfn_query p_query = u ? (pfn_query)(void *)GetProcAddress(u, "QueryDisplayConfig") : nullptr;
        pfn_devinfo p_info = u ? (pfn_devinfo)(void *)GetProcAddress(u, "DisplayConfigGetDeviceInfo") : nullptr;
        pfn_kmt_open p_open = g ? (pfn_kmt_open)(void *)GetProcAddress(g, "D3DKMTOpenAdapterFromGdiDisplayName") : nullptr;
        pfn_kmt_close p_close = g ? (pfn_kmt_close)(void *)GetProcAddress(g, "D3DKMTCloseAdapter") : nullptr;
        if (!p_sizes || !p_query || !p_info) return out;
        UINT32 np = 0, nm = 0;
        if (p_sizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || np == 0) return out;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm ? nm : 1);
        if (p_query(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return out;

        wchar_t primary[32] = {};
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR h, HDC, LPRECT, LPARAM lp) -> BOOL {
            MONITORINFOEXW mi{}; mi.cbSize = sizeof mi;
            if (GetMonitorInfoW(h, &mi) && (mi.dwFlags & MONITORINFOF_PRIMARY))
                wcsncpy_s((wchar_t *)lp, 32, mi.szDevice, _TRUNCATE);
            return TRUE;
        }, (LPARAM)primary);

        for (UINT32 i = 0; i < np; ++i)
        {
            display_info d;
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sn{};
            sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME; sn.header.size = sizeof sn;
            sn.header.adapterId = paths[i].sourceInfo.adapterId; sn.header.id = paths[i].sourceInfo.id;
            if (p_info(&sn.header) == ERROR_SUCCESS) d.gdi = sn.viewGdiDeviceName;
            DISPLAYCONFIG_TARGET_DEVICE_NAME tn{};
            tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME; tn.header.size = sizeof tn;
            tn.header.adapterId = paths[i].targetInfo.adapterId; tn.header.id = paths[i].targetInfo.id;
            if (p_info(&tn.header) == ERROR_SUCCESS) d.friendly = tn.monitorFriendlyDeviceName;
            DISPLAYCONFIG_ADAPTER_NAME an{};
            an.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME; an.header.size = sizeof an;
            an.header.adapterId = paths[i].sourceInfo.adapterId;
            if (p_info(&an.header) == ERROR_SUCCESS)
            {
                const std::wstring ap = an.adapterDevicePath;
                d.vendor = (ap.find(L"VEN_10DE") != std::wstring::npos) ? L"NVIDIA"
                         : (ap.find(L"VEN_8086") != std::wstring::npos) ? L"Intel"
                         : (ap.find(L"VEN_1002") != std::wstring::npos) ? L"AMD" : L"other";
            }
            if (d.friendly.empty()) d.friendly = L"DISPLAY";
            {
                const UINT32 mi = paths[i].sourceInfo.modeInfoIdx;
                if (mi != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && mi < nm && modes[mi].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
                { d.mode_w = modes[mi].sourceMode.width; d.mode_h = modes[mi].sourceMode.height; }
            }
            d.primary = (primary[0] != 0 && d.gdi == primary);
            if (p_open && !d.gdi.empty())
            {
                kmt_open ko{};
                wcsncpy_s(ko.DeviceName, 32, d.gdi.c_str(), _TRUNCATE);
                if (p_open(&ko) == 0)
                {
                    d.adapter = ko.AdapterLuid; d.adapter_ok = true;
                    if (p_close) { kmt_close kc{ ko.hAdapter }; p_close(&kc); }
                    d.adapter_idx = adapter_index_of(d.adapter);
                }
            }
            out.push_back(d);
        }
        return out;
    }

    std::wstring gpu_label(int idx)   // "GPU 1 RTX 5060 TI"
    {
        if (idx < 0 || (size_t)idx >= g_adapters.size()) return tr(L"AN UNKNOWN CARD");
        wchar_t b[200];
        swprintf_s(b, L"GPU %d %s", idx, short_card(g_adapters[(size_t)idx].name).c_str());
        return b;
    }

    // The Diagnose resolution follows this PC until the person picks one with
    // its button: the last launch's render size, else the chosen display's
    // current mode, else the primary display's (0.3.0; it was fixed at 1440P).
    int res_preset(unsigned w, unsigned h)   // the nearest of k_res
    {
        if (h >= 1800) return 3;                    // 3840x2160
        if (h > 1200) return (w >= 3200) ? 2 : 1;   // 3440x1440, 2560x1440
        return 0;                                   // 1920x1080
    }
    void res_follow_pc(unsigned lw, unsigned lh, const std::wstring &chosen_gdi)
    {
        if (g_res_user) return;
        unsigned w = lw, h = lh;
        for (const auto &d : g_displays) if (w == 0 && !chosen_gdi.empty() && d.gdi == chosen_gdi) { w = d.mode_w; h = d.mode_h; }
        for (const auto &d : g_displays) if (w == 0 && d.primary) { w = d.mode_w; h = d.mode_h; }
        if (w != 0 && h != 0) g_res_idx = res_preset(w, h);
    }

    // ---------------- the page model ----------------
    //
    // refresh_page() fills this; WM_PAINT draws it. The verdict colours are
    // decided here, never in the paint code.
    struct line { std::wstring text; COLORREF col = C_FG; };
    struct page_view
    {
        bool have_game = false;
        std::wstring game_name, game_path;
        // last launch
        bool have_launch = false;
        std::wstring when;
        line game_card, game_fps, neural_card, neural_fps, display, display_verdict, footer;
        bool game_bottleneck = false, neural_bottleneck = false;
        LUID game_luid{}, bridge_luid{}; bool game_luid_ok = false, bridge_luid_ok = false;
        bool display_on_bridge = false;
        // layout
        std::wstring chosen_gdi, chosen_name;   // Windows' main display (0.3.0 SCAN: no choice any more)
        bool old_display = false;                // the file on disk has a Display= that SAVE removes
        line verdict_a, verdict_b, verdict_c, verdict_d;
        std::wstring diag_card;    // the card Diagnose ran on (nrbench_result.ini AdapterName)
        std::wstring diag_short;   // one line for the layout: card, res, ms/frame
        // rows
        line install, display_row, diag_status, diag, multi, record, reso, reso_detail, reso_fix;
        bool multi_on = false, record_on = false, fit_on = false;
        int multi_mode = -1;   // DcompMultiDisplay: -1 absent (by detection), 0 never, 1 always
        unsigned src_w = 0, src_h = 0;   // the last launch's render size
    };
    page_view g_view;

    // copy=false: checked in the game folder, never copied by COPY FILES.
    // nvngx_dlssnr.dll is NVIDIA's and is not distributed with the bridge:
    // the person gets it (INSTALL page, steps 2-3) and puts it in <game>\mgpu\.
    struct file_check { const wchar_t *rel; const wchar_t *what; bool required; bool copy; };
    const file_check k_bridge_files[] = {
        { L"nvngx.dll_mgpu_bridge.addon64", L"THE ADD-ON",       true,  true  },
        { L"reshade-shaders\\Shaders\\mgpu_depth_tap.fx", L"MGPU_DEPTH_TAP.FX", true, true },   // Depth=1 needs it (build.yml's set)
        { L"mgpu.ini",                      L"MGPU.INI",         true,  true  },
        { L"gpu1.ini",                      L"GPU1.INI",         true,  true  },
        { L"ReShade2.ini",                  L"RESHADE2.INI",     true,  true  },
        { L"mgpu\\nvngx_dlssnr.dll",        L"NVNGX_DLSSNR.DLL", true,  false },
        { L"README.txt",                    L"README.TXT",       false, true  },
        { L"panel_simple.txt",              L"PANEL_SIMPLE.TXT", false, true  },   // the simple panel's layout (PanelSimple=1); in the zip since the build after 0.5.1
        { L"LICENSE",                       L"LICENSE",          false, true  },
    };
    const wchar_t *const k_url_reshade = L"https://reshade.me/downloads/ReShade_Setup_6.8.0_Addon.exe";
    const wchar_t *const k_url_rhi = L"https://github.com/RankFTW/RHI";

    // Other ReShade add-ons beside ours: every *.addon64 / *.addon in the game
    // folder, and in ReShade.ini's AddonPath when one is set. A general check,
    // not a list of names, so Feeder, RenoDX and anything new are all caught.
    // The launcher only reports them; it never moves or deletes a file.
    void scan_addons(const std::wstring &dir, const std::wstring &tag, std::vector<std::wstring> &out)
    {
        for (const wchar_t *pat : { L"*.addon64", L"*.addon" })
        {
            WIN32_FIND_DATAW fd{};
            HANDLE h = FindFirstFileW(join(dir, pat).c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do
            {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                const std::wstring n = fd.cFileName;
                if (_wcsicmp(n.c_str(), L"nvngx.dll_mgpu_bridge.addon64") == 0) continue;
                // *.addon also matches *.addon64 on some file systems (8.3 names): no duplicates
                bool dup = false;
                for (const auto &o : out) if (_wcsicmp(o.c_str(), (tag + n).c_str()) == 0) dup = true;
                if (!dup) out.push_back(tag + n);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    std::vector<std::wstring> other_addons(const std::wstring &game)
    {
        std::vector<std::wstring> out;
        scan_addons(game, L"", out);
        std::wstring ap = utf8_to_w(ini_get(read_file(join(game, L"ReShade.ini")), "AddonPath"));
        while (!ap.empty() && (ap.back() == L' ' || ap.back() == L'\\' || ap.back() == L'/')) ap.pop_back();
        if (!ap.empty() && ap != L".")
        {
            const std::wstring dir = PathIsRelativeW(ap.c_str()) ? join(game, ap.c_str()) : ap;
            if (dir_exists(dir)) scan_addons(dir, L"ADDONPATH\\", out);
        }
        return out;
    }
    std::wstring join_names(const std::vector<std::wstring> &v)
    {
        std::wstring s;
        for (const auto &n : v) s += (s.empty() ? L"" : L", ") + n;
        return s;
    }
    void open_url(const wchar_t *url) { ShellExecuteW(g_main, L"open", url, nullptr, nullptr, SW_SHOWNORMAL); }

    std::wstring selected_game()
    {
        const int i = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
        if (i < 0 || (size_t)i >= g_library.size()) return L"";
        return g_library[(size_t)i];
    }

    void view_last_launch(const std::wstring &game, page_view &v)
    {
        const std::string t = read_file(join(game, L"mgpu\\last_launch.ini"));
        if (t.empty())
        {
            v.have_launch = false;
            v.game_card = { L"NO LAUNCH RECORDED YET", C_DIM };
            v.neural_card = { L"RUN THE GAME ONCE WITH THE BRIDGE INSTALLED", C_DIM };
            return;
        }
        v.have_launch = true;
        auto g = [&](const char *k) { return utf8_to_w(ini_get(t, k)); };
        v.when = g("When");
        v.src_w = (unsigned)_wtoi(g("SourceWidth").c_str()); v.src_h = (unsigned)_wtoi(g("SourceHeight").c_str());
        v.game_luid_ok = parse_luid(ini_get(t, "GameAdapterLuid"), v.game_luid);
        v.bridge_luid_ok = parse_luid(ini_get(t, "BridgeAdapterLuid"), v.bridge_luid);
        const bool valid = (g("SelectionValid") != L"0");

        const int gi = v.game_luid_ok ? adapter_index_of(v.game_luid) : -1;
        v.game_card = { gi >= 0 ? short_card(g_adapters[(size_t)gi].name) : (v.game_luid_ok ? tr(L"ADAPTER ") + g("GameAdapterLuid") : tr(L"UNKNOWN")), C_FG };
        std::wstring bn = g("BridgeAdapterName");
        if (bn.empty() && v.bridge_luid_ok) { const int bi = adapter_index_of(v.bridge_luid); if (bi >= 0) bn = g_adapters[(size_t)bi].name; }
        v.neural_card = { bn.empty() ? L"NONE" : short_card(bn), bn.empty() ? C_RED : C_FG };
        if (!valid) v.neural_card = { L"NONE - NO CARD WAS SELECTED", C_RED };
        else if (v.game_luid_ok && v.bridge_luid_ok && same_luid(v.game_luid, v.bridge_luid))
            v.neural_card = { short_card(bn) + tr(L" - THE SAME CARD AS THE GAME"), C_RED };

        const std::wstring gf = g("GameFps"), nf = g("NeuralFps");
        if (!gf.empty()) v.game_fps = { fmt0(gf) + L" FPS", C_DIM };
        if (!nf.empty()) v.neural_fps = { fmt0(nf) + L" FPS", C_DIM };
        if (!gf.empty() && !nf.empty())
        {
            const double a = _wtof(gf.c_str()), b = _wtof(nf.c_str());
            if (a > 0.0 && b > 0.0) { v.game_bottleneck = a < b * 0.95; v.neural_bottleneck = b < a * 0.95; }
        }

        const std::wstring dn = g("Display");
        const bool dn_ok = !dn.empty() && dn != L"unknown";
        v.display = { dn_ok ? upper(dn) : L"NOT RECORDED", dn_ok ? C_FG : C_DIM };
        v.display_on_bridge = (g("DisplayOnBridge") == L"1");
        LUID dl{}; const bool dl_ok = parse_luid(ini_get(t, "DisplayAdapterLuid"), dl);
        if (v.display_on_bridge) v.display_verdict = { L"ON THE DLSS 5 CARD", C_FG };
        else if (dl_ok && v.game_luid_ok && same_luid(dl, v.game_luid)) v.display_verdict = { L"ON THE GAME CARD", C_RED };
        else if (dl_ok && valid) v.display_verdict = { L"ON NEITHER CARD", C_RED };
        else if (dn_ok) v.display_verdict = { L"CARD NOT RESOLVED", C_DIM };

        std::wstring f;
        const std::wstring lat = g("SealLatencyMs"), l2 = g("L2Ms"), ev = g("EvaluateMsPerFrame");
        if (!lat.empty()) f += L"SEAL " + fmt1(lat) + L" MS   ";
        if (!l2.empty()) f += L"READY " + fmt1(l2) + L" MS   ";
        if (!ev.empty()) f += L"DLSS 5: " + fmt1(ev) + L" MS/FRAME   ";
        if (!g("Passes").empty()) f += g("Passes") + (g("Passes") == L"1" ? L" PASS   " : L" PASSES   ");
        if (!g("SourceWidth").empty()) f += g("SourceWidth") + L"X" + g("SourceHeight") + L"   ";
        if (lat.empty() && l2.empty() && g("Stage") == L"arm") f = tr(L"ARMED, NO FRAMES RECORDED YET   ");
        f += upper(g("Build"));
        v.footer = { f, C_DIM };
    }

    void view_layout(const std::string &ini, page_view &v)
    {
        (void)ini;
        // 0.3.0 SCAN round (Marcelo): what decides where the picture lands is the
        // card behind Windows' main display, so that display is what is shown.
        const display_info *cd = nullptr;
        for (const auto &d : g_displays) if (d.primary) cd = &d;
        v.chosen_gdi = cd ? cd->gdi : std::wstring();
        v.chosen_name = cd ? upper(cd->friendly) : std::wstring();
        if (v.chosen_name.empty()) v.chosen_name = v.chosen_gdi;

        // A: what the cables say now
        // Every line names where it was read from, so nobody mistakes a
        // cable reading for a game result, or a synthetic run for a launch.
        if (cd == nullptr)
            v.verdict_a = { L"MAIN DISPLAY: NOT FOUND", C_DIM };
        else if (!cd->adapter_ok || cd->adapter_idx < 0)
            v.verdict_a = { tr(L"MAIN DISPLAY: ") + v.chosen_name + tr(L" - ITS CARD COULD NOT BE RESOLVED"), C_RED };
        else if (v.have_launch && v.game_luid_ok && same_luid(cd->adapter, v.game_luid))
            v.verdict_a = { tr(L"MAIN DISPLAY: ") + v.chosen_name + tr(L" IS ON THE CARD THAT RENDERED THE GAME"), C_RED };
        else if (is_igpu(g_adapters[(size_t)cd->adapter_idx]))   // a setup like Dave's; the add-on's guards keep DLSS 5 off it
            v.verdict_a = { tr(L"MAIN DISPLAY: ") + v.chosen_name + tr(L" IS ON THE IGPU"), C_FG };
        else
            v.verdict_a = { tr(L"MAIN DISPLAY: ") + v.chosen_name + tr(L" IS ON ") + gpu_label(cd->adapter_idx), C_FG };

        // B, C, D: what the last launch recorded - the only source that can
        // say where the neural output landed
        if (!v.have_launch)
            v.verdict_b = { L"LAUNCH: NONE YET - RUN THE GAME ONCE, THE ADD-ON RECORDS IT", C_DIM };
        else if (v.display_on_bridge)
            v.verdict_b = { tr(L"LAUNCH: DLSS 5 OUTPUT LANDED ON ") + v.display.text + tr(L" (DLSS 5 CARD)"), C_FG };
        else
        {
            v.verdict_b = { tr(L"LAUNCH: DLSS 5 WAS NOT OFFLOADED TO ") + v.chosen_name, C_RED };
            v.verdict_c = { L"CHANGE WHICH GPU DRIVES YOUR MAIN DISPLAY, THE DESKTOP MODE", C_DIM };
            v.verdict_d = { L"OR THE CABLE, THEN RUN THE GAME AGAIN", C_DIM };
        }
        // the synthetic run, named as such; it never stands in for a launch
        if (!v.diag_short.empty() && v.verdict_c.text.empty())
            v.verdict_c = { tr(L"DIAGNOSE: ") + v.diag_short + tr(L" (SYNTHETIC, NO GAME)"), C_DIM };
    }

    // ---------------- game names (0.3.0) ----------------
    // A game's folder is often Binaries\Win64 or bin\x64: the name is guessed
    // from the first folder up that is not one of those, asked for when the
    // game is added, and can be changed with RENAME. launcher.ini NameN=.
    std::wstring guess_name(const std::wstring &path)
    {
        static const wchar_t *const skip[] = { L"win64", L"win32", L"x64", L"x86", L"bin", L"bin64", L"bin32", L"binaries",
                                               L"retail", L"shipping", L"game", L"release", L"wingdk", L"winstore" };
        std::wstring p = path;
        for (int depth = 0; depth < 4; ++depth)
        {
            while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();   // dirname keeps the slash
            const std::wstring leaf = leafname(p);
            bool s = false;
            for (const wchar_t *k : skip) if (_wcsicmp(leaf.c_str(), k) == 0) s = true;
            const std::wstring up = dirname(p);
            if (!s || up.size() <= 3) return leaf;   // not a skip name, or the drive is next
            p = up;
        }
        return leafname(p);
    }
    std::wstring game_label(size_t i)
    {
        if (i < g_names.size() && !g_names[i].empty()) return g_names[i];
        return i < g_library.size() ? guess_name(g_library[i]) : std::wstring();
    }

    // the working copy of the selected game's mgpu.ini
    bool work_dirty() { return g_work_ini != g_disk_ini; }
    void work_load(const std::wstring &game)
    {
        g_work_game = game;
        g_disk_ini = game.empty() ? std::string() : read_file(join(game, L"mgpu.ini"));
        g_work_ini = g_disk_ini;
        // 0.3.0 SCAN round: the add-on decides. A Display= left by an older
        // launcher is refused by the add-on when it is on the game's card (R269),
        // so the working copy drops it and SAVE writes the file without it.
        if (!ini_get(g_work_ini, "Display").empty() || !ini_get(g_work_ini, "DisplayName").empty())
        {
            g_work_ini = ini_remove(g_work_ini, "Display");
            g_work_ini = ini_remove(g_work_ini, "DisplayName");
        }
    }
    bool work_save()
    {
        if (g_work_game.empty() || !work_dirty()) return true;
        if (!write_file(join(g_work_game, L"mgpu.ini"), g_work_ini)) return false;
        g_disk_ini = g_work_ini;
        return true;
    }

    // a small modal "name" box, built in memory (no resource file)
    struct name_box { std::wstring value; };
    INT_PTR CALLBACK name_box_proc(HWND d, UINT m, WPARAM w, LPARAM l)
    {
        switch (m)
        {
        case WM_INITDIALOG:
            SetWindowLongPtrW(d, DWLP_USER, l);
            SetWindowTextW(d, tr(L"Name this game").c_str());
            SetDlgItemTextW(d, 100, tr(L"The name shown in the library:").c_str());
            SetDlgItemTextW(d, 101, ((name_box *)l)->value.c_str());
            SetDlgItemTextW(d, IDCANCEL, tr(L"Cancel").c_str());
            SetDlgItemTextW(d, IDOK, L"OK");
            SendDlgItemMessageW(d, 101, EM_SETSEL, 0, -1);
            SetFocus(GetDlgItem(d, 101));
            return FALSE;
        case WM_COMMAND:
            if (LOWORD(w) == IDOK)
            {
                name_box *nb = (name_box *)GetWindowLongPtrW(d, DWLP_USER);
                wchar_t b[200] = {}; GetDlgItemTextW(d, 101, b, 200);
                nb->value = b;
                while (!nb->value.empty() && nb->value.back() == L' ') nb->value.pop_back();
                while (!nb->value.empty() && nb->value.front() == L' ') nb->value.erase(0, 1);
                EndDialog(d, 1); return TRUE;
            }
            if (LOWORD(w) == IDCANCEL) { EndDialog(d, 0); return TRUE; }
            break;
        }
        return FALSE;
    }
    bool ask_name(std::wstring &value)
    {
        std::vector<WORD> v;
        auto align = [&]() { if (v.size() % 2) v.push_back(0); };   // DWORD alignment
        auto str = [&](const wchar_t *s) { for (; *s; ++s) v.push_back((WORD)*s); v.push_back(0); };
        auto dw = [&](DWORD x) { v.push_back(LOWORD(x)); v.push_back(HIWORD(x)); };
        dw(DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU); dw(0);
        v.push_back(4); v.push_back(0); v.push_back(0); v.push_back(230); v.push_back(62);
        v.push_back(0); v.push_back(0); str(L"");                     // no menu, default class, title set later
        v.push_back(9); str(L"Segoe UI");
        auto item = [&](DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls)
        {
            align(); dw(WS_CHILD | WS_VISIBLE | style); dw(0);
            v.push_back((WORD)x); v.push_back((WORD)y); v.push_back((WORD)cx); v.push_back((WORD)cy); v.push_back(id);
            v.push_back(0xFFFF); v.push_back(cls); str(L""); v.push_back(0);
        };
        item(SS_LEFT, 7, 7, 216, 10, 100, 0x0082);
        item(WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 7, 19, 216, 13, 101, 0x0081);
        item(BS_DEFPUSHBUTTON | WS_TABSTOP, 119, 40, 50, 14, IDOK, 0x0080);
        item(BS_PUSHBUTTON | WS_TABSTOP, 173, 40, 50, 14, IDCANCEL, 0x0080);
        name_box nb{ value };
        const INT_PTR r = DialogBoxIndirectParamW(g_hinst, (LPCDLGTEMPLATEW)v.data(), g_main, name_box_proc, (LPARAM)&nb);
        if (r != 1) return false;
        value = nb.value;
        return true;
    }

    // ---------------- the options tab (ID_TAB_ADV; "ADVANCED" until 0.3.0) ----------------
    //
    // The same keys the ReShade panel writes (dllmain: ui_ini_write / the
    // write_mode lambda), written the same way, so a value set here and a
    // value set in the panel are one thing. Read at arm: every change takes
    // effect on the next launch, which the page says.
    struct adv_view
    {
        bool tuning = false; int style = 0; double tone = 1.0, structure = 1.0, skin = 1.0; bool mask = true;
        double intensity = 2.0; int passes = 1;
        int fg = 0, keep = 2;
        bool sr = false; bool sr_native = true; int sr_mode = 2; int sr_preset = 0;
        int crop = 0;               // 0 off, 1 on, 2 auto
        bool n16 = false; double n16_power = 2.2;
        int pass2 = 2;              // R320: 0 Medium, 1 Light, 2 Full (no ResidualPass=2 line)
        bool nrs = false;           // R321: NRScale present and above 0.05 (R299's own test)
        // R356: the pass settings, read the way the panel reads them
        int net1 = 0, net2 = 0;     // 0 Light-FP8 (NRPreset<n> absent/0/2), 1 Full-FP8 (NRPreset<n>=1)
        int res1 = 0, res2 = 0;     // 1 Residual Neural, 0 Native
        bool fg1 = false;           // FrameGen=4
        int net1x2 = 0;             // R361: NRPreset1x2 (absent = net1)
    };
    adv_view g_adv;

    void adv_read(const std::string &ini)
    {
        adv_view a;
        auto g = [&](const char *k) { return ini_get(ini, k); };
        auto gi = [&](const char *k, int d) { const std::string v = g(k); return v.empty() ? d : atoi(v.c_str()); };
        auto gf = [&](const char *k, double d) { const std::string v = g(k); return v.empty() ? d : atof(v.c_str()); };
        a.tuning = gi("Tuning", 0) != 0;
        a.style = (int)(gf("Style", 0.0) + 0.5); if (a.style < 0) a.style = 0; if (a.style > 2) a.style = 2;
        a.tone = gf("ToneStrength", 1.0); a.structure = gf("StructureStrength", 1.0); a.skin = gf("SkinStrength", 1.0);
        a.mask = gi("AutoMask", 1) != 0;
        a.intensity = gf("Intensity", 2.0);
        a.passes = (gi("Passes", 1) >= 2) ? 2 : 1;
        a.fg = gi("NRFrameFilter", 0); if (a.fg < 0 || a.fg > 3) a.fg = 0;
        a.keep = gi("NRFrameKeep", 2); if (a.keep < 2 || a.keep > 8) a.keep = 2;
        a.sr = gi("SRUpscale", 0) == 1;
        a.sr_native = gi("SRScale", 0) == 0;
        a.sr_mode = gi("SRQuality", 2); if (a.sr_mode < 0 || a.sr_mode > 2) a.sr_mode = 2;
        a.sr_preset = gi("SRPreset", 0);
        {
            const std::string sm = g("ScaleMode");
            const bool on = (!sm.empty() && (sm[0] == '1' || sm[0] == '2' || sm.compare(0, 4, "crop") == 0 || sm.compare(0, 5, "whole") == 0));
            const std::string ca = g("CropAuto");
            const bool autoc = !(ca.empty() || ca[0] == '0' || _strnicmp(ca.c_str(), "off", 3) == 0 || _strnicmp(ca.c_str(), "false", 5) == 0);
            a.crop = on ? (autoc ? 2 : 1) : 0;
        }
        a.n16 = gi("NRInput16", 0) != 0;
        a.n16_power = gf("NRInput16Power", 2.2);
        // R320: read the way the panel reads it (R319)
        a.pass2 = (gi("ResidualPass", -1) == 2) ? ((gi("ResidualRender", 0) == 1) ? 1 : 0) : 2;
        a.nrs = gi("NRS", 0) == 1;   // R357: our residual on pass 1 (was NRScale, R321)
        {   // R356
            const int fb = gi("NRPreset", 0);
            a.net1 = (gi("NRPreset1", fb) == 1) ? 1 : 0;
            a.net2 = (gi("NRPreset2", fb) == 1) ? 1 : 0;
            const bool nrs1 = gi("NRS", 0) == 1;
            a.res1 = nrs1 ? 1 : 0;
            const std::string p2 = g("Pass2");
            a.res2 = (nrs1 && !p2.empty()) ? ((p2[0] == 'n' || p2[0] == 'N') ? 1 : 0) : ((gi("ResidualPass", -1) == 2) ? 1 : 0);
            a.fg1 = gi("FrameGen", 0) == 4;
            a.net1x2 = (gi("NRPreset1x2", a.net1) == 1) ? 1 : 0;   // R361
        }
        g_adv = a;
    }

    std::string f2(double v) { char b[32]; snprintf(b, sizeof b, "%.2f", v); return b; }
    std::wstring wf2(double v) { return utf8_to_w(f2(v)); }

    // one change: a list of key=value pairs written together (the SR pair
    // is never written alone - the panel's rule)
    void adv_write(const std::vector<std::pair<const char *, std::string>> &kv)
    {
        const std::wstring game = selected_game();
        if (game.empty()) return;
        std::string ini = g_work_ini;   // the working copy: SAVE writes it
        if (ini.empty()) { tr_box(g_main, L"This game has no mgpu.ini yet. Install the bridge first.", L"Options", MB_OK | MB_ICONWARNING); return; }
        for (const auto &p : kv) ini = ini_set(ini, p.first, p.second);
        g_work_ini = ini;
        adv_read(g_work_ini);
        if (g_btn_save) InvalidateRect(g_btn_save, nullptr, TRUE);
    }
    void adv_sr_mode_pairs(std::vector<std::pair<const char *, std::string>> &kv, int mode, bool native)
    {
        // dllmain's write_mode, verbatim in effect
        kv.push_back({ "SRQuality", std::to_string(mode) });
        if (native) { kv.push_back({ "SRScale", "0" }); kv.push_back({ "SRMvLowRes", "0" }); }
        else
        {
            const int scale = (mode == 2) ? 67 : ((mode == 1) ? 58 : 50);
            kv.push_back({ "SRScale", std::to_string(scale) }); kv.push_back({ "SRMvLowRes", "1" });
        }
    }
    double step(double v, double d, double lo, double hi)
    {
        v += d;
        if (v < lo) v = lo; if (v > hi) v = hi;
        return (double)((long long)(v * 100.0 + (v >= 0 ? 0.5 : -0.5))) / 100.0;
    }

    void refresh_adv();   // below
    void set_tab(int t);  // below

    void adv_click(int id)
    {
        using KV = std::vector<std::pair<const char *, std::string>>;
        const adv_view &a = g_adv;
        KV kv;
        switch (id)
        {
        case ID_ADV_TUNING_OFF: kv.push_back({ "Tuning", "0" }); break;
        case ID_ADV_TUNING_ON:  kv.push_back({ "Tuning", "1" }); break;
        case ID_ADV_STYLE_A: kv.push_back({ "Style", "0.0" }); break;
        case ID_ADV_STYLE_B: kv.push_back({ "Style", "1.0" }); break;
        case ID_ADV_STYLE_C: kv.push_back({ "Style", "2.0" }); break;
        case ID_ADV_TONE_M:   kv.push_back({ "ToneStrength", f2(step(a.tone, -0.1, 0, 2)) }); break;
        case ID_ADV_TONE_P:   kv.push_back({ "ToneStrength", f2(step(a.tone, +0.1, 0, 2)) }); break;
        case ID_ADV_STRUCT_M: kv.push_back({ "StructureStrength", f2(step(a.structure, -0.1, 0, 2)) }); break;
        case ID_ADV_STRUCT_P: kv.push_back({ "StructureStrength", f2(step(a.structure, +0.1, 0, 2)) }); break;
        case ID_ADV_SKIN_M:   kv.push_back({ "SkinStrength", f2(step(a.skin, -0.1, 0, 2)) }); break;
        case ID_ADV_SKIN_P:   kv.push_back({ "SkinStrength", f2(step(a.skin, +0.1, 0, 2)) }); break;
        case ID_ADV_MASK_OFF: kv.push_back({ "AutoMask", "0" }); break;
        case ID_ADV_MASK_ON:  kv.push_back({ "AutoMask", "1" }); break;
        case ID_ADV_INT_M: kv.push_back({ "Intensity", f2(step(a.intensity, -0.1, 0, 2)) }); break;
        case ID_ADV_INT_P: kv.push_back({ "Intensity", f2(step(a.intensity, +0.1, 0, 2)) }); break;
        case ID_ADV_PASSES_1: kv.push_back({ "Passes", "1" }); break;
        case ID_ADV_PASSES_2: kv.push_back({ "Passes", "2" }); break;
        case ID_ADV_FG_OFF:  kv.push_back({ "NRFrameFilter", "0" }); break;
        case ID_ADV_FG_MAP:  kv.push_back({ "NRFrameFilter", "1" }); break;
        case ID_ADV_FG_FIX:  kv.push_back({ "NRFrameFilter", "2" }); break;
        case ID_ADV_FG_THIN: kv.push_back({ "NRFrameFilter", "3" }); break;
        case ID_ADV_KEEP_M: kv.push_back({ "NRFrameKeep", std::to_string(a.keep > 2 ? a.keep - 1 : 2) }); break;
        case ID_ADV_KEEP_P: kv.push_back({ "NRFrameKeep", std::to_string(a.keep < 8 ? a.keep + 1 : 8) }); break;
        // R357: OPTIMIZATION's NRS is OUR residual on pass 1 (NRS=1, Marcelo 23:54), not NRScale. One choice at a
        // time stays: OFF and the two upscaling choices also write NRS=0 (the residual refuses SRUpscale anyway).
        case ID_ADV_SR_OFF: kv.push_back({ "SRUpscale", "0" }); break;   // R358: DLSS ON GPU 1 (OLD) off - NRS untouched
        case ID_ADV_SR_ON:  kv.push_back({ "SRUpscale", "1" }); adv_sr_mode_pairs(kv, a.sr_mode, a.sr_native); kv.push_back({ "NRS", "0" }); break;
        case ID_ADV_SR_NATIVE: kv.push_back({ "SRUpscale", "1" }); adv_sr_mode_pairs(kv, a.sr_mode, true);  kv.push_back({ "NRScale", "0" }); kv.push_back({ "NRS", "0" }); break;   // R329
        case ID_ADV_SR_EXP:    kv.push_back({ "SRUpscale", "1" }); adv_sr_mode_pairs(kv, a.sr_mode, false); kv.push_back({ "NRScale", "0" }); kv.push_back({ "NRS", "0" }); break;   // R329
        case ID_ADV_SRQ_Q: adv_sr_mode_pairs(kv, 2, a.sr_native); break;
        case ID_ADV_SRQ_B: adv_sr_mode_pairs(kv, 1, a.sr_native); break;
        case ID_ADV_SRQ_P: adv_sr_mode_pairs(kv, 0, a.sr_native); break;
        case ID_ADV_PRESET_0: kv.push_back({ "SRPreset", "0" }); break;
        case ID_ADV_PRESET_K: kv.push_back({ "SRPreset", "11" }); break;
        case ID_ADV_PRESET_L: kv.push_back({ "SRPreset", "12" }); break;
        case ID_ADV_PRESET_M: kv.push_back({ "SRPreset", "13" }); break;
        case ID_ADV_CROP_OFF:  kv.push_back({ "ScaleMode", "0" }); break;
        case ID_ADV_CROP_ON:   kv.push_back({ "CropAuto", "0" }); kv.push_back({ "ScaleMode", "1" }); break;
        case ID_ADV_CROP_AUTO: kv.push_back({ "CropAuto", "1" }); kv.push_back({ "ScaleMode", "1" }); break;
        case ID_ADV_N16_OFF: kv.push_back({ "NRInput16", "0" }); break;
        case ID_ADV_N16_ON:  kv.push_back({ "NRInput16", "4" }); break;
        case ID_ADV_N16P_M: kv.push_back({ "NRInput16Power", f2(step(a.n16_power, -0.1, 1, 3)) }); break;
        case ID_ADV_N16P_P: kv.push_back({ "NRInput16Power", f2(step(a.n16_power, +0.1, 1, 3)) }); break;
        // R320: the same keys the panel's "2nd pass (x2)" writes (R319)
        case ID_ADV_P2_MED:   kv.push_back({ "ResidualPass", "2" }); kv.push_back({ "ResidualScale", "0.75" }); kv.push_back({ "ResidualRender", "0" }); break;
        case ID_ADV_P2_LIGHT: kv.push_back({ "ResidualPass", "2" }); kv.push_back({ "ResidualScale", "0.75" }); kv.push_back({ "ResidualRender", "1" }); break;
        case ID_ADV_P2_FULL:  kv.push_back({ "ResidualPass", "0" }); break;
        // R321: the panel's "Pass 1 scale (NRS)" switch, same keys
        case ID_ADV_NRS_ON:  kv.push_back({ "NRS", "1" }); kv.push_back({ "NRScale", "0" }); kv.push_back({ "SRUpscale", "0" }); break;   // R357: our path; R329: one at a time
        case ID_ADV_NRS_OFF: kv.push_back({ "NRS", "0" }); kv.push_back({ "NRScale", "0" }); break;   // R358: OPTIMIZATION OFF
        // R356: the same keys the panel's "Pass settings" writes
        case ID_ADV_P1N_LIGHT: kv.push_back({ "NRPreset1", "0" }); break;
        case ID_ADV_P1N_FULL:  kv.push_back({ "NRPreset1", "1" }); break;
        case ID_ADV_P2N_LIGHT: kv.push_back({ "NRPreset2", "0" }); break;
        case ID_ADV_P2N_FULL:  kv.push_back({ "NRPreset2", "1" }); break;
        case ID_ADV_P1R_RES:    kv.push_back({ "NRS", "1" }); kv.push_back({ "NRScale", "0" }); kv.push_back({ "SRUpscale", "0" }); break;   // R357: = OPTIMIZATION's NRS
        case ID_ADV_P1R_NATIVE: kv.push_back({ "NRS", "0" }); break;
        case ID_ADV_P2R_RES:
            if (a.res1) kv.push_back({ "Pass2", "nrs" });
            else { kv.push_back({ "ResidualPass", "2" }); kv.push_back({ "ResidualScale", "0.75" }); kv.push_back({ "ResidualRender", "0" }); }
            break;
        case ID_ADV_P2R_NATIVE:
            if (a.res1) kv.push_back({ "Pass2", "full" });
            else kv.push_back({ "ResidualPass", "0" });
            break;
        case ID_ADV_FG1_OFF: kv.push_back({ "FrameGen", "0" }); break;
        case ID_ADV_FG1_ON:  kv.push_back({ "FrameGen", "4" }); break;
        case ID_ADV_P1X2_LIGHT: kv.push_back({ "NRPreset1x2", "0" }); break;   // R361
        case ID_ADV_P1X2_FULL:  kv.push_back({ "NRPreset1x2", "1" }); break;
        case ID_ADV_BEST:    // Marcelo's default (R360/R361): Full-FP8 + Residual on pass 1 at x1 and on the last pass; Light-FP8 on pass 1 at x2+
            kv.push_back({ "NRS", "1" }); kv.push_back({ "NRPreset1", "1" }); kv.push_back({ "NRPreset1x2", "0" }); kv.push_back({ "NRPreset2", "1" });
            kv.push_back({ "Pass2", "nrs" }); kv.push_back({ "FrameGen", "0" });
            kv.push_back({ "NRScale", "0" }); kv.push_back({ "SRUpscale", "0" });   // R357: one optimization at a time
            break;
        default: return;
        }
        adv_write(kv);
        refresh_adv();
    }

    void refresh_page()
    {
        page_view v;
        const std::wstring game = selected_game();
        v.have_game = !game.empty();
        // another game, or no unsaved change: read the file again (COPY FILES may
        // have made it, or it was edited outside)
        if (_wcsicmp(game.c_str(), g_work_game.c_str()) != 0 || !work_dirty()) work_load(game);
        for (HWND h : { g_btn_install, g_btn_logs, g_btn_change, g_btn_res, g_btn_passes, g_btn_multi, g_btn_record, g_btn_fit, g_btn_save, g_btn_rename })
            if (h) EnableWindow(h, v.have_game);
        if (v.have_game)
        {
            {
                const int sel = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
                v.game_name = upper(sel >= 0 ? game_label((size_t)sel) : leafname(game));
            }
            v.game_path = upper(game);

            // install
            const bool reshade = file_exists(join(game, L"dxgi.dll")) || file_exists(join(game, L"d3d11.dll")) || file_exists(join(game, L"d3d12.dll"));
            std::wstring missing;
            bool only_nr = true;
            for (const auto &f : k_bridge_files)
                if (f.required && !file_exists(join(game, f.rel)))
                {
                    missing += (missing.empty() ? L"" : L", ") + tr(f.what);
                    if (f.copy) only_nr = false;
                }
            const std::vector<std::wstring> others = other_addons(game);
            if (!reshade) v.install = { L"RESHADE NOT FOUND - SEE THE INSTALL PAGE, STEP 1", C_RED };
            else if (!missing.empty() && only_nr) v.install = { L"MISSING NVNGX_DLSSNR.DLL - SEE THE INSTALL PAGE, STEPS 2 AND 3", C_RED };
            else if (!missing.empty()) v.install = { tr(L"MISSING ") + missing + tr(L" - PRESS COPY FILES"), C_RED };
            else if (!others.empty()) v.install = { tr(L"OTHER ADD-ONS: ") + upper(join_names(others)) + tr(L" - SEE THE INSTALL PAGE, STEP 5"), C_RED };
            else v.install = { L"OK", C_FG };

            const std::string ini = g_work_ini;
            view_last_launch(game, v);

            // launch record row (R270b): off by default; the add-on turns it
            // off itself and leaves the reason in LaunchRecordOff=
            const std::wstring lr = utf8_to_w(ini_get(ini, "LaunchRecord")), lro = upper(utf8_to_w(ini_get(ini, "LaunchRecordOff")));
            v.record_on = (lr == L"1");
            if (v.record_on) v.record = { L"ON - FPS PER CARD WRITTEN WHILE THE GAME RUNS", C_FG };
            else if (!lro.empty()) v.record = { tr(L"OFF - TURNED ITSELF OFF: ") + lro, C_RED };
            else v.record = { L"OFF (DEFAULT) - FPS PER CARD ONLY AT THE END OF A BOUNDED RUN", C_DIM };

            // diagnose rows (read before the layout: the layout names the card
            // Diagnose ran on when there is no launch to name it)
            const std::string br = read_file(join(game, L"nrbench_result.ini"));
            if (br.empty())
            {
                v.diag_status = { L"NOT RUN YET", C_DIM };
            }
            else
            {
                auto g = [&](const char *k) { return utf8_to_w(ini_get(br, k)); };
                v.diag_status = { L"SYNTHETIC RUN, NO GAME", C_DIM };
                v.diag_card = g("AdapterName");
                if (g("Result") != L"ok")
                    v.diag = { L"THE CARD COULD NOT RUN THE DLSS 5 MODEL - SEE NRBENCH_LOG.TXT", C_RED };
                else
                {
                    v.diag_short = short_card(v.diag_card) + L" " + g("Width") + L"X" + g("Height") + L" " + g("Passes") + L"P " + fmt1(g("FrameMeanMs")) + L" MS/FRAME";
                    // R320: a residual 2nd pass says its size
                    const std::wstring p2 = (g("Pass2Mode") == L"residual") ? tr(L" (2ND PASS ") + g("Pass2Width") + L"X" + g("Pass2Height") + L")" : L"";
                    v.diag_short += p2;
                    v.diag = { tr(L"SYNTHETIC: ") + short_card(v.diag_card) + L"  " + g("Width") + L"X" + g("Height") + L"  " + g("Passes") + (g("Passes") == L"1" ? tr(L" PASS  ") : tr(L" PASSES  "))
                               + fmt1(g("FrameMeanMs")) + tr(L" MS/FRAME  FITS ") + g("BudgetFps") + L" FPS" + p2, C_FG };
                }
            }
            view_layout(ini, v);

            // resolution check (R271): the last launch's render size against the
            // main display's current mode. What is on screen in that case has
            // not been observed, so the row states the two sizes and nothing more.
            const std::wstring fit = utf8_to_w(ini_get(ini, "DcompFit"));
            v.fit_on = (fit == L"1");   // absent / auto / 2 = the guard; 0 = never
            {
                const display_info *cd = nullptr;
                for (const auto &d : g_displays) if (!v.chosen_gdi.empty() && d.gdi == v.chosen_gdi) cd = &d;
                wchar_t b[200];
                if (!v.have_launch || v.src_w == 0 || v.src_h == 0)
                    v.reso = { L"NO LAUNCH RECORDED - NOTHING TO COMPARE YET", C_DIM };
                else if (cd == nullptr || cd->mode_w == 0)
                    v.reso = { L"MAIN DISPLAY NOT FOUND - NOTHING TO COMPARE", C_DIM };
                else if (v.src_w > cd->mode_w || v.src_h > cd->mode_h)
                {
                    v.reso = { L"MISMATCH RESOLUTION FOUND", C_RED };
                    swprintf_s(b, tr(L"GAME RENDERED %uX%u, %s IS %uX%u").c_str(), v.src_w, v.src_h, v.chosen_name.c_str(), cd->mode_w, cd->mode_h);
                    v.reso_detail = { b, C_RED };
                    v.reso_fix = { L"TEST BORDERLESS OR FULLSCREEN (GAME DEPENDENT)", C_DIM };
                }
                else
                {
                    swprintf_s(b, tr(L"OK - GAME %uX%u ON %s %uX%u").c_str(), v.src_w, v.src_h, v.chosen_name.c_str(), cd->mode_w, cd->mode_h);
                    v.reso = { b, C_FG };
                }
                if (v.fit_on) v.reso_fix = { L"FORCE FIT IS ON: THE PICTURE IS ALWAYS SCALED TO THE GAME WINDOW", C_FG };
            }
            res_follow_pc(v.have_launch ? v.src_w : 0u, v.have_launch ? v.src_h : 0u, v.chosen_gdi);

            // display row
            v.old_display = !ini_get(g_disk_ini, "Display").empty() && ini_get(g_work_ini, "Display").empty();
            if (v.old_display) v.display_row = { L"OLD DISPLAY= LINE IN MGPU.INI - SAVE REMOVES IT", C_RED };
            else if (v.chosen_gdi.empty()) v.display_row = { L"THE ADD-ON DECIDES - MAIN DISPLAY NOT FOUND", C_DIM };
            else v.display_row = { tr(L"THE ADD-ON DECIDES - MAIN DISPLAY: ") + v.chosen_name, C_FG };

            // multi-display row (the key is off by default; not measured)
            const std::wstring md = utf8_to_w(ini_get(ini, "DcompMultiDisplay"));
            v.multi_mode = (md == L"1") ? 1 : (md == L"0") ? 0 : -1;
            v.multi_on = (v.multi_mode == 1);
            if (v.multi_mode == -1) v.multi = { L"BY DETECTION (DEFAULT)", C_DIM };
            else if (v.multi_mode == 1) v.multi = { L"ALWAYS - NO DETECTION", C_FG };
            else v.multi = { L"NEVER - THE OLD GATE", C_FG };

        }
        g_view = v;
        if (v.have_game) adv_read(g_work_ini); else g_adv = adv_view();
        if (g_btn_save) InvalidateRect(g_btn_save, nullptr, TRUE);
        if (g_btn_multi) SetWindowTextW(g_btn_multi, v.multi_mode == 1 ? L"MULTI-DISPLAY: ON" : L"MULTI-DISPLAY: OFF");
        if (g_btn_record) SetWindowTextW(g_btn_record, v.record_on ? L"TURN OFF" : L"TURN ON");
        if (g_btn_fit) SetWindowTextW(g_btn_fit, v.fit_on ? L"FORCE FIT: ON" : L"IF OUTPUT RESIZE IS NOT WORKING: FORCE FIT");
        if (g_btn_res) SetWindowTextW(g_btn_res, k_res_label[g_res_idx]);
        InvalidateRect(g_main, nullptr, TRUE);
        for (HWND h : { g_btn_multi, g_btn_record, g_btn_fit, g_btn_res }) if (h) InvalidateRect(h, nullptr, TRUE);
        refresh_adv();
    }

    void fill_displays()
    {
        g_adapters = enum_adapters();
        g_displays = enum_displays();
    }

    // ---------------- actions ----------------
    // before the selected game changes (or the window closes): unsaved changes
    // are saved, dropped, or the change is cancelled (false)
    bool confirm_leave()
    {
        if (g_work_game.empty() || !work_dirty()) return true;
        std::wstring name = leafname(g_work_game);
        for (size_t i = 0; i < g_library.size(); ++i) if (_wcsicmp(g_library[i].c_str(), g_work_game.c_str()) == 0) name = game_label(i);
        wchar_t b[1200];
        swprintf_s(b, tr(L"The changes for %s are not saved.\n\nSave them to %s?").c_str(), name.c_str(), join(g_work_game, L"mgpu.ini").c_str());
        const int r = MessageBoxW(g_main, b, tr(L"Save").c_str(), MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return false;
        if (r == IDYES && !work_save())
        {
            tr_box(g_main, L"mgpu.ini could not be written.", L"Save", MB_OK | MB_ICONWARNING);
            return false;
        }
        work_load(g_work_game);   // saved, or dropped
        return true;
    }
    void do_save()
    {
        if (g_work_game.empty()) return;
        if (!work_save()) tr_box(g_main, L"mgpu.ini could not be written.", L"Save", MB_OK | MB_ICONWARNING);
        refresh_page();
    }
    void do_rename()
    {
        const int i = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
        if (i < 0 || (size_t)i >= g_library.size()) return;
        std::wstring name = game_label((size_t)i);
        if (!ask_name(name)) return;
        if (g_names.size() < g_library.size()) g_names.resize(g_library.size());
        g_names[(size_t)i] = name;
        save_config();
        InvalidateRect(g_list, nullptr, TRUE);
        refresh_page();
    }
    void do_add_game()
    {
        if (!confirm_leave()) return;
        IFileDialog *dlg = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
        DWORD opt = 0; dlg->GetOptions(&opt);
        dlg->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dlg->SetTitle(tr(L"Pick the game folder (where the game's exe and ReShade are)").c_str());
        if (SUCCEEDED(dlg->Show(g_main)))
        {
            IShellItem *it = nullptr;
            if (SUCCEEDED(dlg->GetResult(&it)))
            {
                PWSTR p = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)))
                {
                    std::wstring path = p; CoTaskMemFree(p);
                    bool dup = false;
                    for (const auto &g : g_library) if (_wcsicmp(g.c_str(), path.c_str()) == 0) dup = true;
                    if (!dup)
                    {
                        std::wstring name = guess_name(path);
                        ask_name(name);   // cancel keeps the guess
                        g_library.push_back(path);
                        if (g_names.size() < g_library.size() - 1) g_names.resize(g_library.size() - 1);
                        g_names.push_back(name);
                        save_config();
                        SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)path.c_str());
                        SendMessageW(g_list, LB_SETCURSEL, (WPARAM)(g_library.size() - 1), 0);
                    }
                }
                it->Release();
            }
        }
        dlg->Release();
        refresh_page();
    }

    void do_remove_game()
    {
        const int i = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
        if (i < 0 || (size_t)i >= g_library.size()) return;
        g_library.erase(g_library.begin() + i);
        if ((size_t)i < g_names.size()) g_names.erase(g_names.begin() + i);
        work_load(L"");   // its unsaved changes go with it
        save_config();
        SendMessageW(g_list, LB_DELETESTRING, (WPARAM)i, 0);
        refresh_page();
    }

    void do_install()
    {
        const std::wstring game = selected_game();
        if (game.empty()) return;
        const std::wstring payload = join(g_exe_dir, L"payload");
        if (!dir_exists(payload))
        {
            tr_box(g_main, L"The launcher's payload\\ folder is missing. Put the bridge release files in it (the add-on, mgpu.ini, gpu1.ini, ReShade2.ini, README.txt, LICENSE).", L"Copy files", MB_OK | MB_ICONWARNING);
            return;
        }
        if (!(file_exists(join(game, L"dxgi.dll")) || file_exists(join(game, L"d3d11.dll")) || file_exists(join(game, L"d3d12.dll"))))
        {
            if (tr_box(g_main, L"ReShade was not found in this folder. The bridge is a ReShade add-on and needs ReShade (with add-on support) installed first.\n\nCopy the bridge files anyway?", L"Copy files", MB_YESNO | MB_ICONQUESTION) != IDYES)
                return;
        }
        CreateDirectoryW(join(game, L"mgpu").c_str(), nullptr);
        CreateDirectoryW(join(game, L"reshade-shaders").c_str(), nullptr);
        CreateDirectoryW(join(game, L"reshade-shaders\\Shaders").c_str(), nullptr);
        std::wstring report;
        int copied = 0, kept = 0, missing = 0;
        for (const auto &f : k_bridge_files)
        {
            if (!f.copy) continue;   // checked below, never copied
            const std::wstring src = join(payload, f.rel), dst = join(game, f.rel);
            if (!file_exists(src)) { if (f.required) { report += tr(L"missing in payload: ") + f.rel + L"\r\n"; ++missing; } continue; }
            if ((wcscmp(f.rel, L"mgpu.ini") == 0 || wcscmp(f.rel, L"gpu1.ini") == 0) && file_exists(dst)) { ++kept; continue; }
            if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) ++copied;
            else { report += tr(L"could not copy: ") + f.rel + L"\r\n"; ++missing; }
        }
        // R320a (Marcelo 2026-10-16 15:14): an existing mgpu.ini is REPLACED by the release's,
        // so an upgrade gets every 0.4.0 default. The only lines carried over are the Dcomp*
        // keys - the display/composition preference the person chose. Before replacing, the
        // person is asked: Yes = replace and keep a backup (mgpu.ini.bak_<date>_<time>),
        // No = replace with no backup, Cancel = keep the current mgpu.ini as it is.
        {
            const std::wstring gi_path = join(game, L"mgpu.ini");
            const std::string pay = read_file(join(payload, L"mgpu.ini"));
            const std::string cur = read_file(gi_path);
            if (!pay.empty() && !cur.empty() && pay != cur)
            {
                const int ans = tr_box(g_main, L"This game already has an mgpu.ini. Replace it with the one in this release? Your Dcomp settings are kept.\n\nYes: replace it and keep a backup of the old file.\nNo: replace it with no backup.\nCancel: keep the current mgpu.ini.",
                                       L"Copy files", MB_YESNOCANCEL | MB_ICONQUESTION);
                if (ans == IDYES || ans == IDNO)
                {
                    bool ok = true;
                    if (ans == IDYES)
                    {
                        SYSTEMTIME t{}; GetLocalTime(&t);
                        wchar_t bn[64];
                        swprintf_s(bn, L"mgpu.ini.bak_%04u%02u%02u_%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
                        if (CopyFileW(gi_path.c_str(), join(game, bn).c_str(), TRUE)) report += tr(L"Backup of the old mgpu.ini: ") + bn + L"\r\n";
                        else { report += tr(L"The backup could not be written - mgpu.ini was not replaced.") + L"\r\n"; ++missing; ok = false; }
                    }
                    if (ok)
                    {
                        // carry the Dcomp* lines (active ones only) into the release's file
                        std::string out = pay, kept_keys;
                        size_t pos = 0;
                        while (pos < cur.size())
                        {
                            size_t eol = cur.find('\n', pos);
                            if (eol == std::string::npos) eol = cur.size();
                            std::string ln = cur.substr(pos, eol - pos);
                            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
                            const size_t eq = ln.find('=');
                            if (ln.compare(0, 5, "Dcomp") == 0 && eq != std::string::npos && eq > 5)
                            {
                                const std::string k = ln.substr(0, eq), v = ln.substr(eq + 1);
                                out = ini_set(out, k.c_str(), v);
                                kept_keys += (kept_keys.empty() ? "" : ", ") + k + "=" + v;
                            }
                            pos = eol + 1;
                        }
                        if (write_file(gi_path, out))
                        {
                            report += tr(L"mgpu.ini replaced with the release's.") + L"\r\n";
                            if (!kept_keys.empty()) report += tr(L"Kept: ") + utf8_to_w(kept_keys) + L"\r\n";
                            if (kept > 0) --kept;   // counted as kept in the loop above
                        }
                        else { report += tr(L"mgpu.ini could not be written.") + L"\r\n"; ++missing; }
                    }
                }
            }
        }
        std::wstring after;
        bool warn = missing != 0;
        if (!file_exists(join(game, L"mgpu\\nvngx_dlssnr.dll")))
        {
            after += tr(L"\r\nnvngx_dlssnr.dll is not in the game's mgpu folder. It is not included with the bridge. "
                        L"You can get it from RHI (github.com/RankFTW/RHI) or from the RenoDX Discord. Put it in the mgpu folder. See the INSTALL page, steps 2 and 3.\r\n");
            warn = true;
        }
        const std::vector<std::wstring> others = other_addons(game);
        if (!others.empty())
        {
            after += tr(L"\r\nOther ReShade add-ons found: ") + join_names(others) + tr(L"\r\nMove them out of the game folder. See the INSTALL page, step 5.\r\n");
            warn = true;
        }
        wchar_t b[400];
        swprintf_s(b, tr(L"Copied %d file(s), kept %d existing config file(s), %d problem(s).\r\n%s").c_str(), copied, kept, missing, report.c_str());
        const std::wstring msg = std::wstring(b) + after;
        tr_box(g_main, msg.c_str(), L"Copy files", MB_OK | (warn ? MB_ICONWARNING : MB_ICONINFORMATION));
        refresh_page();
    }

    void do_open_logs()
    {
        const std::wstring game = selected_game();
        if (game.empty()) return;
        const std::wstring d = join(game, L"mgpu");
        ShellExecuteW(g_main, L"open", dir_exists(d) ? d.c_str() : game.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void do_multi_toggle()
    {
        const std::wstring game = selected_game();
        if (game.empty()) return;
        std::string ini = g_work_ini;   // the working copy: SAVE writes it
        if (ini.empty()) { tr_box(g_main, L"This game has no mgpu.ini yet. Install the bridge first.", L"Multi-display", MB_OK | MB_ICONWARNING); return; }
        // 0.3.0 (Marcelo): ON / OFF from what this game's mgpu.ini holds. ON writes
        // DcompMultiDisplay=1 (and DcompOverlay=1, which it needs). OFF removes the
        // key - nothing is written, so the add-on's default (detection, R273)
        // applies. A 0 (the old gate) reads as OFF; pressing turns it ON.
        const int next = (g_view.multi_mode == 1) ? -1 : 1;
        if (next == 1 && tr_box(g_main, L"Always: the composition mode runs with any number of displays, without checking which card the game window's display is on. If that display is on the card that renders the game, the picture travels back across the bus.\n\nThe default already composes by itself when the game window is on the DLSS 5 card.\n\nSet Always for this game?", L"Multi-display", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK)
            return;
        if (next == 1) { ini = ini_set(ini, "DcompOverlay", "1"); ini = ini_set(ini, "DcompMultiDisplay", "1"); }
        else ini = ini_remove(ini, "DcompMultiDisplay");
        g_work_ini = ini;
        refresh_page();
    }

    void do_record_toggle()
    {
        const std::wstring game = selected_game();
        if (game.empty()) return;
        std::string ini = g_work_ini;   // the working copy: SAVE writes it
        if (ini.empty()) { tr_box(g_main, L"This game has no mgpu.ini yet. Install the bridge first.", L"Launch record", MB_OK | MB_ICONWARNING); return; }
        const bool on = !g_view.record_on;
        if (on && tr_box(g_main, L"With LaunchRecord=1 the add-on refreshes mgpu\\last_launch.ini every 300 frames while the game runs, with the frame rate of each card. It is written on the bridge thread with the lock released. If one write takes more than 2 ms the add-on turns it off by itself and the next launch runs without it.\n\nTurn it on for this game?", L"Launch record", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
            return;
        ini = ini_set(ini, "LaunchRecord", on ? "1" : "0");
        if (on) ini = ini_set(ini, "LaunchRecordOff", "");
        g_work_ini = ini;
        refresh_page();
    }

    void do_fit_toggle()
    {
        const std::wstring game = selected_game();
        if (game.empty()) return;
        std::string ini = g_work_ini;   // the working copy: SAVE writes it
        if (ini.empty()) { tr_box(g_main, L"This game has no mgpu.ini yet. Install the bridge first.", L"Force fit", MB_OK | MB_ICONWARNING); return; }
        const bool on = !g_view.fit_on;
        if (on && tr_box(g_main, L"Are you sure? This option has not been tested.\n\nPlease test fullscreen and borderless first (which one works is game dependent). The add-on already tries to fit the picture by itself when it sees the mismatch (DcompFit=auto); Force fit makes it scale always.\n\nWhat it does: the compositor scales the DLSS 5 picture to the game window instead of showing it 1:1. The DLSS 5 stage still runs at the game's render size. Takes effect on the next launch.", L"Force fit - not tested", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return;
        ini = ini_set(ini, "DcompFit", on ? "1" : "auto");
        g_work_ini = ini;
        refresh_page();
    }

    // ---------------- Diagnose (nrcheck --bench) ----------------
    // (k_res, k_res_label: with the globals at the top, refresh_page uses them)

    int dxgi_index_of(const LUID &l)
    {
        IDXGIFactory1 *f = nullptr;
        if (FAILED(create_dxgi_factory1(&f))) return -1;
        int found = -1;
        for (UINT i = 0; found < 0; ++i)
        {
            IDXGIAdapter1 *a = nullptr;
            if (f->EnumAdapters1(i, &a) != S_OK) break;
            DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
            if (same_luid(d.AdapterLuid, l)) found = (int)i;
            a->Release();
        }
        f->Release();
        return found;
    }

    void do_diagnose()
    {
        const std::wstring game = selected_game();
        if (game.empty()) return;
        // the card, in order: the bridge's adapter from the last launch (what the
        // add-on actually chose), else the only NVIDIA card that does not drive
        // Windows' main display (the game lands on the main display's card).
        // Anything less clear: no guess, the person runs the game once.
        int idx = -1; std::wstring which;
        {
            const std::string ll = read_file(join(game, L"mgpu\\last_launch.ini"));
            LUID l{};
            if (parse_luid(ini_get(ll, "BridgeAdapterLuid"), l))
            {
                idx = dxgi_index_of(l);
                which = utf8_to_w(ini_get(ll, "BridgeAdapterName")) + tr(L" (the card the bridge chose on the last launch)");
            }
        }
        if (idx < 0)
        {
            LUID main{}; bool main_ok = false;
            for (const auto &d : g_displays) if (d.primary && d.adapter_ok) { main = d.adapter; main_ok = true; }
            int pick = -1, n = 0;
            for (size_t i = 0; i < g_adapters.size(); ++i)
            {
                const adapter_info &a = g_adapters[i];
                if (a.vendor != 0x10DE || is_igpu(a)) continue;   // DLSS-NR runs on NVIDIA
                if (main_ok && same_luid(a.luid, main)) continue;
                pick = (int)i; ++n;
            }
            if (n == 1)
            {
                idx = dxgi_index_of(g_adapters[(size_t)pick].luid);
                which = g_adapters[(size_t)pick].name + tr(L" (the only NVIDIA card that does not drive the main display)");
            }
        }
        if (idx < 0)
        {
            tr_box(g_main, L"Diagnose cannot tell which card does the DLSS 5 work. The last launch did not record a card, and there is not exactly one NVIDIA card besides the one that drives the main display.\n\nRun the game once with the bridge installed, then press SCAN DISPLAY again.", L"Diagnose", MB_OK | MB_ICONINFORMATION);
            return;
        }
        const std::wstring exe = join(game, L"nrcheck.exe"), dll = join(game, L"nvngx.dll_nrcheck.dll");
        // R320: the payload's copy wins whenever it is there, so the game folder never keeps
        // an older nrcheck that does not know --pass2-scale (it would bench the native pass).
        {
            const std::wstring pe = join(g_exe_dir, L"payload\\tools\\nrcheck.exe"), pd = join(g_exe_dir, L"payload\\tools\\nvngx.dll_nrcheck.dll");
            if (file_exists(pe) && file_exists(pd)) { CopyFileW(pe.c_str(), exe.c_str(), FALSE); CopyFileW(pd.c_str(), dll.c_str(), FALSE); }
        }
        if (!file_exists(exe) || !file_exists(dll))
        {
            const std::wstring pe = join(g_exe_dir, L"payload\\tools\\nrcheck.exe"), pd = join(g_exe_dir, L"payload\\tools\\nvngx.dll_nrcheck.dll");
            if (!file_exists(pe) || !file_exists(pd))
            {
                tr_box(g_main, L"nrcheck.exe and nvngx.dll_nrcheck.dll were not found in the game folder or in the launcher's payload\\tools\\ folder.", L"Diagnose", MB_OK | MB_ICONWARNING);
                return;
            }
            CopyFileW(pe.c_str(), exe.c_str(), FALSE);
            CopyFileW(pd.c_str(), dll.c_str(), FALSE);
        }
        if (!file_exists(join(game, L"mgpu\\nvngx_dlssnr.dll")))
        {
            tr_box(g_main, L"mgpu\\nvngx_dlssnr.dll is not in this game folder. Diagnose needs it (it is the DLSS 5 model). See the INSTALL page, steps 2 and 3.", L"Diagnose", MB_OK | MB_ICONWARNING);
            return;
        }
        wchar_t cmd[1024];
        swprintf_s(cmd, L"\"%s\" --bench --neural %d --other -1 --res %s --passes %d --frames 300", exe.c_str(), idx, k_res[g_res_idx], g_passes);
        // R320: the 2nd pass the game would run. Medium and Light both bench at ResidualScale:
        // with no game there is no DLSS mode, so Light has no render size to follow - the
        // add-on does the same when the game has no DLSS table.
        if (g_passes == 2 && g_adv.pass2 != 2 && !g_adv.nrs)   // R321: with NRS on pass 1 the residual 2nd pass is off
        {
            double p2 = atof(ini_get(g_work_ini, "ResidualScale").c_str());
            if (p2 < 0.30 || p2 > 1.00) p2 = 0.75;
            wchar_t extra[64]; swprintf_s(extra, L" --pass2-scale %.2f", p2);
            wcscat_s(cmd, extra);
        }
        std::wstring msg;
        {
            wchar_t mb[1200];
            swprintf_s(mb, tr(L"Diagnose runs DLSS-NR (mgpu\\nvngx_dlssnr.dll) on \"%s\" at %s with %d pass(es), about 10 seconds, no game. Close the game first.\n\nRun it now?").c_str(),
                       which.c_str(), k_res[g_res_idx], g_passes);
            msg = mb;
        }
        if (tr_box(g_main, msg.c_str(), L"Diagnose", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
        g_view.diag_status = { L"RUNNING..", C_FG };
        InvalidateRect(g_main, nullptr, TRUE); UpdateWindow(g_main);
        STARTUPINFOW si{}; si.cb = sizeof si; PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, game.c_str(), &si, &pi))
        {
            g_view.diag_status = { L"NRCHECK.EXE COULD NOT BE STARTED", C_RED };
            InvalidateRect(g_main, nullptr, TRUE);
            return;
        }
        // wait without freezing the window. If the window is closed meanwhile,
        // the quit must not be swallowed here: stop waiting, re-post the quit,
        // and let the main loop exit - otherwise the process lingers with no
        // window, holding its own folder open. The bench is left to finish by
        // itself (seconds); it is never killed.
        bool quit = false;
        for (;;)
        {
            const DWORD w = MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, INFINITE, QS_ALLINPUT);
            if (w == WAIT_OBJECT_0) break;
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
            {
                if (m.message == WM_QUIT) { quit = true; break; }
                TranslateMessage(&m); DispatchMessageW(&m);
            }
            if (quit) break;
        }
        DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        if (quit) { PostQuitMessage(0); return; }
        refresh_page();
        if (code != 0)
        {
            wchar_t b[200]; swprintf_s(b, tr(L"Diagnose ended with code %lu. nrbench_log.txt in the game folder says why.").c_str(), (unsigned long)code);
            tr_box(g_main, b, L"Diagnose", MB_OK | MB_ICONWARNING);
        }
    }

    // ---------------- the page ----------------
    void paint_sweep(HDC dc)
    {
        // screen.cpp's bar: period 180 frames, a fifth of the width, four
        // segments with a falling ramp. The hue never moves.
        const float phase = (float)(g_frame % 180ull) / 180.0f;
        const int bar_w = WIN_W / 5, bar_x = (int)(phase * (float)(WIN_W + bar_w)) - bar_w;
        fill(dc, 0, Y_SWEEP, WIN_W, 3, C_BASE);
        for (int k = 0; k < 4; ++k)
        {
            float t = (float)(k + 1) / 5.0f;
            t = t * t * (3.0f - 2.0f * t);
            const float a = (t < 0.5f ? t * 2.0f : (1.0f - t) * 2.0f) * 0.9f;
            const COLORREF c = RGB((int)(8 + 48 * a), (int)(8 + 53 * a), (int)(9 + 60 * a));
            const int seg = bar_w / 4;
            int x = bar_x + k * seg, w = seg;
            if (x < 0) { w += x; x = 0; }
            if (x + w > WIN_W) w = WIN_W - x;
            fill(dc, x, Y_SWEEP, w, 3, c);
        }
    }

    void paint_picture(HDC dc, const page_view &v)
    {
        const int x0 = PX, y0 = Y_PIC, w = PW;
        const int n = (int)g_adapters.size();
        bool unknown = false;
        for (const auto &d : g_displays) if (!d.adapter_ok || d.adapter_idx < 0) unknown = true;
        const int cols = n + (unknown ? 1 : 0);
        if (cols == 0) { textf(dc, x0 + 20, y0 + 20, w - 40, L"NO GRAPHICS ADAPTER FOUND", 2, C_DIM); return; }
        const int gapx = 24;
        int bw = (w - 40 - gapx * (cols - 1)) / cols; if (bw > 250) bw = 250;
        const int total = bw * cols + gapx * (cols - 1);
        const int cx0 = x0 + (w - total) / 2;
        const int gy = y0 + 132, gh = 62, my = y0 + 14, mh = 52;

        const int game_i = (v.have_launch && v.game_luid_ok) ? adapter_index_of(v.game_luid) : -1;
        const int neural_i = (v.have_launch && v.bridge_luid_ok) ? adapter_index_of(v.bridge_luid) : -1;

        for (int c = 0; c < cols; ++c)
        {
            const int x = cx0 + c * (bw + gapx);
            const bool is_unknown = (c >= n);
            std::wstring l1, l2; COLORREF col = C_DIM;
            if (is_unknown) { l1 = L"UNKNOWN CARD"; l2 = L"NOT IN THE DXGI TABLE"; }
            else
            {
                const adapter_info &a = g_adapters[(size_t)c];
                wchar_t b[32]; swprintf_s(b, L"GPU %d  ", c);
                l1 = std::wstring(b) + short_card(a.name);
                // the role and where it was read from: the launch names both
                // cards; with no launch, Diagnose names only the card it ran on
                if (c == neural_i) { l2 = L"DLSS 5 - LAUNCH"; col = C_FG; }
                else if (c == game_i) l2 = L"GAME - LAUNCH";
                else if (!v.have_launch && !v.diag_card.empty() && upper(a.name) == upper(v.diag_card)) { l2 = L"DLSS 5 - DIAGNOSE"; col = C_FG; }
                else if (is_igpu(a)) l2 = L"IGPU";
            }
            frame(dc, x, gy, bw, gh, 3, col);
            textf(dc, x + 10, gy + 12, bw - 20, l1, 2, col);
            if (!l2.empty()) text_cf(dc, x + bw / 2, gy + 34, bw - 20, l2, 2, col);
            for (int i = 0; i < 6 && 10 + i * 12 + 6 < bw - 10; ++i) fill(dc, x + 10 + i * 12, gy + gh - 10, 6, 5, col);

            std::vector<const display_info *> ds;
            for (const auto &d : g_displays)
            {
                const bool here = is_unknown ? (!d.adapter_ok || d.adapter_idx < 0) : (d.adapter_ok && d.adapter_idx == c);
                if (here) ds.push_back(&d);
            }
            if (ds.empty()) continue;
            const int k = (int)ds.size();
            int mw = (bw - 8 * (k - 1)) / k; if (mw > 150) mw = 150;
            const int mx0 = x + (bw - (mw * k + 8 * (k - 1))) / 2;
            for (int i = 0; i < k; ++i)
            {
                const display_info &d = *ds[(size_t)i];
                const bool lit = !v.chosen_gdi.empty() && d.gdi == v.chosen_gdi;
                const COLORREF mc = lit ? C_LIT : C_DIM;
                const int mx = mx0 + i * (mw + 8);
                frame(dc, mx, my, mw, mh, 3, mc);
                textf(dc, mx + 8, my + 12, mw - 16, upper(d.friendly), 2, mc);
                if (d.primary) textf(dc, mx + 8, my + 30, mw - 16, L"PRIMARY", 2, C_DIM);
                const int sx = mx + mw / 2;
                fill(dc, sx - 2, my + mh, 4, 8, mc);
                fill(dc, sx - 14, my + mh + 8, 28, 3, mc);
                fill(dc, sx - 1, my + mh + 11, lit ? 4 : 2, gy - (my + mh + 11), lit ? C_LIT : C_RULE);
            }
        }
        int y = y0 + 200;
        textf(dc, x0 + 20, y, w - 40, v.verdict_a.text, 2, v.verdict_a.col); y += 19;
        textf(dc, x0 + 20, y, w - 40, v.verdict_b.text, 2, v.verdict_b.col); y += 19;
        textf(dc, x0 + 20, y, w - 40, v.verdict_c.text, 2, v.verdict_c.col); y += 19;
        textf(dc, x0 + 20, y, w - 40, v.verdict_d.text, 2, v.verdict_d.col);
    }

    // The options tab's rows: label, then the value and the buttons flowing
    // from ADV_VX; a row whose buttons do not fit the page continues on a
    // second line (relayout_adv), and the rows below move down. One entry per
    // row, in the order build_ui creates the buttons: ctl = how many of
    // g_adv_ctls are the row's; val = what is drawn before them.
    // (0.3.0: the section moved from y 70 to 80 and the rows from 104 to 112 -
    // at 70 the section label sat on the game name drawn at 60)
    // (SAVE round: the section to 104 and the rows to 136, under the SAVES TO line)
    const int ADV_Y0 = 136, ADV_ROW = 30, ADV_VX = PX + 300;
    enum : int { AV_NONE, AV_TEXT, AV_NUM };   // AV_TEXT: a sentence (tuning); AV_NUM: a number before - / +
    struct adv_row_def { const wchar_t *label; int ctl; int val; };
    const adv_row_def k_adv_rows[] = {
        { L"TUNING", 2, AV_TEXT },     { L"MODEL STYLE", 3, AV_NONE },   { L"TONE", 2, AV_NUM },
        { L"STRUCTURE", 2, AV_NUM },   { L"SKIN", 2, AV_NUM },           { L"AUTO MASK", 2, AV_NONE },
        { L"INTENSITY", 2, AV_NUM },   { L"PASSES", 2, AV_NONE },
        // R356 (rows 8-13): the panel's "Pass settings"
        { L"PASS 1 NETWORK", 2, AV_NONE },  { L"PASS 1 RESOLUTION", 2, AV_NONE },   // R362: no "AT X2" row (noisy); Best writes NRPreset1x2
        { L"PASS 2 NETWORK", 2, AV_NONE },  { L"PASS 2 RESOLUTION", 2, AV_NONE },
        { L"DLSS 5 CARD FRAME GEN", 2, AV_NONE }, { L"DEFAULTS", 1, AV_NONE },
        { L"FRAME GENERATION", 4, AV_NONE },
        // R329: the 2nd pass choice is the panel's Experimental setting only. R358: OPTIMIZATION = our NRS pass;
        // DLSS ON GPU 1 (OLD) is its own row again. Picking one of the DLSS choices writes NRS=0 (the residual
        // refuses SRUpscale), PERFORMANCE writes SRUpscale=0: one optimization at a time, as R329.
        // R358 (Marcelo 00:02): OPTIMIZATION is OFF | PERFORMANCE (our NRS pass, NRS=1); the old DLSS-on-GPU-1
        // choices are their own row, DLSS ON GPU 1 (OLD).
        { L"KEEP 1 GENERATED FRAME IN", 2, AV_NUM },  { L"OPTIMIZATION", 2, AV_NONE },
        { L"DLSS ON GPU 1 (OLD)", 3, AV_NONE },
        { L"DLSS MODE", 3, AV_NONE },  { L"DLSS PRESET", 4, AV_NONE },   { L"CROP", 3, AV_NONE },
        { L"16-BIT COLOUR", 2, AV_NONE }, { L"16-BIT POWER", 2, AV_NUM },
    };
    const int ADV_N = (int)(sizeof(k_adv_rows) / sizeof(k_adv_rows[0]));
    int g_adv_y[ADV_N] = {}, g_adv_vw[ADV_N] = {};   // each row's y and its value's width (relayout_adv)
    int g_adv_end = ADV_Y0;                          // under the last row
    const wchar_t *const k_tuning_on = L"ON - THE MODEL CONTROLS BELOW APPLY";
    const wchar_t *const k_tuning_off = L"OFF - MODEL DEFAULTS";
    // the value drawn before row i's buttons, and its colour (as 0.5.0 drew them)
    line adv_value(int i)
    {
        const adv_view &a = g_adv;
        switch (i)
        {
        case 0:  return { a.tuning ? k_tuning_on : k_tuning_off, a.tuning ? C_FG : C_DIM };
        case 2:  return { wf2(a.tone), a.tuning ? C_FG : C_DIM };
        case 3:  return { wf2(a.structure), a.tuning ? C_FG : C_DIM };
        case 4:  return { wf2(a.skin), a.tuning ? C_FG : C_DIM };
        case 6:  return { wf2(a.intensity), C_FG };
        case 15: return { std::to_wstring(a.keep), (a.fg == 3) ? C_FG : C_DIM };   // R329: rows renumbered; R356: +6 (R362: the R361 row is gone)
        case 22: return { wf2(a.n16_power), a.n16 ? C_FG : C_DIM };   // R358: +1 (DLSS ON GPU 1 (OLD))
        default: return { L"", C_DIM };
        }
    }
    int guide_para(HDC dc, int x, int y, int maxw, const std::wstring &s0, COLORREF c);   // below
    void paint_adv(HDC dc)
    {
        section(dc, PX, 104, PW, L"OPTIONS  SAME KEYS THE RESHADE PANEL WRITES - NEXT LAUNCH");
        for (int i = 0; i < ADV_N; ++i)
        {
            textf(dc, PX, g_adv_y[i] + 5, ADV_VX - PX - 12, k_adv_rows[i].label, 2, C_DIM);
            const line v = adv_value(i);
            if (!v.text.empty()) textf(dc, ADV_VX, g_adv_y[i] + 5, g_adv_vw[i], v.text, 2, v.col);
        }
        int y = g_adv_end + 6;
        y = guide_para(dc, PX, y, PW, L"FULL-FP8 ON THE LAST PASS REMOVES THE STROBING WHEN STRUCTURE IS HIGH.", C_DIM);   // R356
        y = guide_para(dc, PX, y, PW, L"FRAME GENERATION NEEDS MVEC=3 (THE GAME'S OWN VECTORS).", C_DIM);
        y = guide_para(dc, PX, y, PW, L"FRAME GENERATION SETTINGS MAY CRASH THE GAME AT STARTUP.", C_RED);
        {   // R329: OPTIMIZATION's state in words and colour: OFF (red) QUALITY MAX (green), or ON FPS MAX (green)
            const bool on = g_view.have_game && (g_adv.nrs || g_adv.sr);
            const bool lit = g_view.have_game;
            const std::wstring w0 = L"OPTIMIZATION", w1 = on ? L"ON" : L"OFF", w2 = on ? L"FPS MAX" : L"QUALITY MAX";
            const int gap = 12;
            int x = PX;
            textf(dc, x, y + 4, PW, w0, 2, C_DIM);                                 x += text_w(w0, 2) + gap;
            textf(dc, x, y + 4, PW - (x - PX), w1, 2, !lit ? C_DIM : on ? C_GRN : C_RED); x += text_w(w1, 2) + gap;
            textf(dc, x, y + 4, PW - (x - PX), w2, 2, lit ? C_GRN : C_DIM);
        }
    }

    // ---- the INSTALL page (the install guide; ID_TAB_GUIDE). Readable
    // without a game selected. Words wrap at the page width.
    const int GUIDE_Y0 = 104;
    // the paragraph is shown once, as a whole (translated, or English in upper
    // case); its pieces are drawn and measured as they are (raw), so a word
    // that happens to be a key is not translated again.
    int raw_w(const std::wstring &s, int px) { return gdi_text_w(s, px); }
    void raw_text(HDC dc, int x, int y, const std::wstring &s, int px, COLORREF c) { gdi_text(dc, x, y, s, px, c); }
    int guide_para(HDC dc, int x, int y, int maxw, const std::wstring &s0, COLORREF c)
    {
        const std::wstring s = shown(s0);
        std::vector<std::wstring> words;
        {
            std::wstring w;
            for (wchar_t ch : s) { if (ch == L' ') { if (!w.empty()) words.push_back(w); w.clear(); } else w += ch; }
            if (!w.empty()) words.push_back(w);
        }
        std::wstring line;
        auto flush = [&]() { if (!line.empty()) { raw_text(dc, x, y, line, 2, c); y += 20; line.clear(); } };
        for (const auto &w : words)
        {
            const std::wstring cand = line.empty() ? w : line + L" " + w;
            if (raw_w(cand, 2) <= maxw) { line = cand; continue; }
            flush();
            if (raw_w(w, 2) <= maxw) { line = w; continue; }
            // wider than the line (Chinese and Japanese have no spaces): break by character
            for (wchar_t ch : w)
            {
                const std::wstring t2 = line + ch;
                if (raw_w(t2, 2) > maxw && !line.empty()) { flush(); line = std::wstring(1, ch); }
                else line = t2;
            }
        }
        flush();
        return y;
    }
    // y of each step's button row, set by paint_guide (buttons follow the text)
    int g_guide_y_reshade = GUIDE_Y0, g_guide_y_rhi = GUIDE_Y0;
    void paint_guide(HDC dc)
    {
        int y = GUIDE_Y0;
        section(dc, PX, y - 40, PW, L"INSTALL GUIDE");
        const int sx = PX + 40, sw = PW - 40;
        text(dc, PX, y, L"1.", 2, C_FG);
        y = guide_para(dc, sx, y, sw, L"Install ReShade with add-on support.", C_FG);
        g_guide_y_reshade = y + 2; y += 34;
        text(dc, PX, y, L"2.", 2, C_FG);
        y = guide_para(dc, sx, y, sw, L"Get nvngx_dlssnr.dll. I do not distribute this file. You can get it from RHI (github.com/RankFTW/RHI) or from the RenoDX Discord.", C_FG);
        g_guide_y_rhi = y + 2; y += 34;
        text(dc, PX, y, L"3.", 2, C_FG);
        y = guide_para(dc, sx, y, sw, L"Put nvngx_dlssnr.dll in the mgpu folder inside the game folder.", C_FG);
        y += 14;
        text(dc, PX, y, L"4.", 2, C_FG);
        y = guide_para(dc, sx, y, sw, L"Press COPY FILES on the Status page. It copies the add-on and its settings files.", C_FG);
        y += 14;
        text(dc, PX, y, L"5.", 2, C_FG);
        y = guide_para(dc, sx, y, sw, L"Move any other ReShade add-ons (.addon64 or .addon files) out of the game folder. The Status page lists the ones it finds.", C_FG);
    }

    void paint_page(HDC dc)
    {
        const page_view &v = g_view;
        fill(dc, 0, 0, WIN_W, WIN_H, C_BASE);
        section(dc, LX, 20, LW, L"LIBRARY");
        if (g_library.empty())
        {
            textf(dc, LX, 60, LW, L"ADD THE GAME FOLDER:", 2, C_DIM);
            textf(dc, LX, 80, LW, L"THE ONE WITH THE GAME'S EXE", 2, C_DIM);
            textf(dc, LX, 100, LW, L"AND RESHADE IN IT", 2, C_DIM);
        }
        text(dc, PX, 18, L"MGPU BRIDGE", 4, C_FG);
        {
            const int x = PX + text_w(L"MGPU BRIDGE", 4) + 24;
            textf(dc, x, 28, g_tabs_x - 10 - x, L"LAUNCHER 0.4.0", 2, C_DIM);
        }
        if (g_tab == 2) { paint_guide(dc); paint_sweep(dc); return; }
        if (!v.have_game)
        {
            textf(dc, PX, 66, PW, L"NO GAME SELECTED", 2, C_DIM);
            paint_sweep(dc);
            return;
        }
        textf(dc, PX, 60, g_name_btn_x - 10 - PX, v.game_name, 2, C_FG);
        {   // where SAVE writes, and whether there is something to save
            const bool dirty = work_dirty();
            const std::wstring st = dirty ? L"UNSAVED CHANGES" : L"SAVED";
            const int sw = text_w(st, 2);
            textf(dc, PX, 80, PW - sw - 20, tr(L"SAVES TO: ") + upper(join(selected_game(), L"mgpu.ini")), 2, C_DIM);
            text(dc, PX + PW - sw, 80, st, 2, dirty ? C_RED : C_DIM);
        }
        if (g_tab == 1) { paint_adv(dc); paint_sweep(dc); return; }

        // ---- last launch ----
        section(dc, PX, Y_LL, PW, v.have_launch ? tr(L"LAST LAUNCH  ") + v.when + tr(L"  THE GAME RUN, RECORDED BY THE ADD-ON") : std::wstring(L"LAST LAUNCH"));
        const int lx = PX, vx = PX + 110, fx = PX + 430;
        int y = Y_LL + 36;
        textf(dc, lx, y + 4, vx - lx - 10, L"GAME", 2, C_DIM);
        textf(dc, vx, y, v.have_launch ? fx - vx - 10 : PW - 110, v.game_card.text, v.have_launch ? 3 : 2, v.game_card.col);
        if (!v.game_fps.text.empty()) { text(dc, fx, y, v.game_fps.text, 3, v.game_fps.col); if (v.game_bottleneck) textf(dc, fx + 130, y + 4, PX + PW - fx - 130, L"BOTTLENECK", 2, C_DIM); }
        y += 34;
        textf(dc, lx, y + 4, vx - lx - 10, L"DLSS 5", 2, C_DIM);
        textf(dc, vx, y, v.have_launch ? fx - vx - 10 : PW - 110, v.neural_card.text, v.have_launch ? 3 : 2, v.neural_card.col);
        if (!v.neural_fps.text.empty()) { text(dc, fx, y, v.neural_fps.text, 3, v.neural_fps.col); if (v.neural_bottleneck) textf(dc, fx + 130, y + 4, PX + PW - fx - 130, L"BOTTLENECK", 2, C_DIM); }
        y += 34;
        if (v.have_launch)
        {
            textf(dc, lx, y + 4, vx - lx - 10, L"DISPLAY", 2, C_DIM);
            textf(dc, vx, y, fx - vx - 10, v.display.text, 3, v.display.col);
            textf(dc, fx, y + 4, PX + PW - fx, v.display_verdict.text, 2, v.display_verdict.col);
            y += 30;
            textf(dc, lx, y + 2, PW, v.footer.text, 2, v.footer.col);
        }

        // ---- layout ----
        section(dc, PX, Y_LAYOUT, PW, L"LAYOUT  CABLES READ FROM THIS PC NOW, ROLES FROM THE LAUNCH");
        paint_picture(dc, v);

        // ---- rows ----
        // one line per R_ index; a value ends at its row's buttons (g_row_end);
        // a label column widens when a language's labels need it
        fill(dc, PX, Y_ROWS, PW, 1, C_RULE);
        const int c1 = PX + label_col({ L"INSTALL", L"DISPLAY", L"DIAGNOSE", L"RESOLUTION" }, 130);
        const int c2 = PX + label_col({ L"MULTI-DISPLAY", L"LAUNCH RECORD" }, 180);
        auto lab = [&](int r, const wchar_t *s, int cx) { textf(dc, PX, st_txt_y(r), cx - PX - 12, s, 2, C_DIM); };
        auto val = [&](int r, int x, const line &ln) { textf(dc, x, st_txt_y(r), g_row_end[r] - x, ln.text, 2, ln.col); };
        lab(R_INSTALL, L"INSTALL", c1);
        text_2(dc, c1, st_txt_y(R_INSTALL), g_row_end[R_INSTALL] - c1, PX + PW - c1, v.install.text, v.install.col);
        lab(R_DISPLAY, L"DISPLAY", c1);       val(R_DISPLAY, c1, v.display_row);
        lab(R_DIAG, L"DIAGNOSE", c1);         // a long status (an error) continues on the diag line when that line is empty
        if (v.diag.text.empty()) text_2(dc, c1, st_txt_y(R_DIAG), g_row_end[R_DIAG] - c1, PX + PW - c1, v.diag_status.text, v.diag_status.col);
        else val(R_DIAG, c1, v.diag_status);
        val(R_DIAGLINE, PX, v.diag);
        lab(R_RESO, L"RESOLUTION", c1);       val(R_RESO, c1, v.reso);
        val(R_RESO_DETAIL, PX, v.reso_detail);
        val(R_RESO_FIX, PX, v.reso_fix);
        lab(R_MULTI, L"MULTI-DISPLAY", c2);   val(R_MULTI, c2, v.multi);
        lab(R_RECORD, L"LAUNCH RECORD", c2);
        text_2(dc, c2, st_txt_y(R_RECORD), g_row_end[R_RECORD] - c2, PX + PW - c2, v.record.text, v.record.col);
        paint_sweep(dc);
    }

    // ---------------- owner-drawn controls ----------------
    // The language flags, drawn with plain GDI shapes (no image files).
    // English shows the United Kingdom flag.
    void poly(HDC dc, const POINT *p, int n, COLORREF c)
    {
        HBRUSH b = CreateSolidBrush(c); HPEN pe = CreatePen(PS_SOLID, 1, c);
        HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pe);
        Polygon(dc, p, n);
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(pe);
    }
    void disc(HDC dc, int cx, int cy, int r, COLORREF c)
    {
        HBRUSH b = CreateSolidBrush(c); HPEN pe = CreatePen(PS_SOLID, 1, c);
        HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pe);
        Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(pe);
    }
    void thick_line(HDC dc, int x0, int y0, int x1, int y1, int w, COLORREF c)
    {
        HPEN pe = CreatePen(PS_SOLID, w, c);
        HGDIOBJ op = SelectObject(dc, pe);
        MoveToEx(dc, x0, y0, nullptr); LineTo(dc, x1, y1);
        SelectObject(dc, op); DeleteObject(pe);
    }
    void draw_flag(HDC dc, int x, int y, int w, int h, int lang)
    {
        const int sd = SaveDC(dc);
        IntersectClipRect(dc, x, y, x + w, y + h);
        const int cx = x + w / 2, cy = y + h / 2;
        switch (lang)
        {
        case LANG_EN:   // United Kingdom
        {
            const COLORREF blue = RGB(1, 33, 105), red = RGB(200, 16, 46), white = RGB(255, 255, 255);
            fill(dc, x, y, w, h, blue);
            thick_line(dc, x, y, x + w, y + h, 5, white); thick_line(dc, x + w, y, x, y + h, 5, white);
            thick_line(dc, x, y, x + w, y + h, 2, red);   thick_line(dc, x + w, y, x, y + h, 2, red);
            fill(dc, cx - 4, y, 8, h, white); fill(dc, x, cy - 4, w, 8, white);
            fill(dc, cx - 2, y, 4, h, red);   fill(dc, x, cy - 2, w, 4, red);
            break;
        }
        case LANG_PT:   // Brazil
        {
            fill(dc, x, y, w, h, RGB(0, 156, 59));
            const POINT d[4] = { { x + 3, cy }, { cx, y + 2 }, { x + w - 3, cy }, { cx, y + h - 2 } };
            poly(dc, d, 4, RGB(255, 223, 0));
            disc(dc, cx, cy, h / 4, RGB(0, 39, 118));
            break;
        }
        case LANG_ES:   // Spain
            fill(dc, x, y, w, h, RGB(170, 21, 27));
            fill(dc, x, y + h / 4, w, h / 2, RGB(241, 191, 0));
            break;
        case LANG_ZH:   // China
        {
            fill(dc, x, y, w, h, RGB(238, 28, 37));
            POINT s[10];
            const int sx = x + 8, sy = y + 8, ro = 6, ri = 2;
            for (int k = 0; k < 10; ++k)
            {
                const double a = -3.14159265358979 / 2.0 + k * 3.14159265358979 / 5.0;
                const int r = (k % 2) ? ri : ro;
                s[k].x = sx + (int)(r * cos(a)); s[k].y = sy + (int)(r * sin(a));
            }
            poly(dc, s, 10, RGB(255, 222, 0));
            break;
        }
        case LANG_JA:   // Japan
            fill(dc, x, y, w, h, RGB(255, 255, 255));
            disc(dc, cx, cy, h * 3 / 10, RGB(188, 0, 45));
            break;
        case LANG_KO:   // South Korea
        {
            fill(dc, x, y, w, h, RGB(255, 255, 255));
            const int r = h / 4;
            disc(dc, cx, cy, r, RGB(0, 71, 160));
            fill(dc, cx - r - 1, cy - r - 1, 2 * r + 3, r + 1, RGB(255, 255, 255));   // keep the lower half blue
            HBRUSH b = CreateSolidBrush(RGB(205, 46, 58)); HPEN pe = CreatePen(PS_SOLID, 1, RGB(205, 46, 58));
            HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pe);
            Pie(dc, cx - r, cy - r, cx + r + 1, cy + r + 1, cx + r, cy, cx - r, cy);   // upper half red
            SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(pe);
            const COLORREF k = RGB(0, 0, 0);
            for (int i = 0; i < 3; ++i)
            {
                fill(dc, x + 3, y + 3 + i * 2, 6, 1, k); fill(dc, x + w - 9, y + 3 + i * 2, 6, 1, k);
                fill(dc, x + 3, y + h - 8 + i * 2, 6, 1, k); fill(dc, x + w - 9, y + h - 8 + i * 2, 6, 1, k);
            }
            break;
        }
        default: break;
        }
        RestoreDC(dc, sd);
    }
    void draw_flag_button(const DRAWITEMSTRUCT *di, int lang)
    {
        HDC dc = di->hDC;
        const RECT &r = di->rcItem;
        const int w = r.right - r.left, h = r.bottom - r.top;
        fill(dc, r.left, r.top, w, h, C_BASE);
        draw_flag(dc, r.left + 3, r.top + 3, w - 6, h - 6, lang);
        const bool on = (lang == g_lang);
        frame(dc, r.left, r.top, w, h, on ? 2 : 1, on ? C_LIT : ((di->itemState & ODS_FOCUS) ? C_FG : C_RULE));
    }
    void draw_button(const DRAWITEMSTRUCT *di)
    {
        HDC dc = di->hDC;
        const RECT &r = di->rcItem;
        const bool down = (di->itemState & ODS_SELECTED) != 0, off = (di->itemState & ODS_DISABLED) != 0;
        wchar_t t[64] = {}; GetWindowTextW(di->hwndItem, t, 64);
        const int id = (int)di->CtlID;
        const int w = r.right - r.left, h = r.bottom - r.top;
        if (id >= ID_LANG_0 && id <= ID_LANG_LAST) { draw_flag_button(di, id - ID_LANG_0); return; }
        bool active = false;
        for (int a : g_active_ids) if (a == id) active = true;
        fill(dc, r.left, r.top, w, h, (down || active) ? C_SEL : C_BASE);
        // R356: the pass settings' choices carry their colour - green FPS+, red FPS-; bright when selected
        const bool grn = (id == ID_ADV_P1N_LIGHT || id == ID_ADV_P2N_LIGHT || id == ID_ADV_P1R_RES || id == ID_ADV_P2R_RES || id == ID_ADV_P1X2_LIGHT);
        const bool red = (id == ID_ADV_P1N_FULL || id == ID_ADV_P2N_FULL || id == ID_ADV_P1R_NATIVE || id == ID_ADV_P2R_NATIVE || id == ID_ADV_P1X2_FULL);
        if ((grn || red) && !off)
        {
            const COLORREF c = active ? (grn ? C_GRN : C_RED) : (grn ? C_GRN_D : C_RED_D);
            frame(dc, r.left, r.top, w, h, active ? 2 : 1, c);
            text_cf(dc, r.left + w / 2, text_y_centred(r.top + h / 2, 2), w - 8, t, 2, active ? c : (grn ? C_GRN_D : C_RED_D));
            return;
        }
        frame(dc, r.left, r.top, w, h, 1, off ? C_RULE : (active ? C_LIT : ((di->itemState & ODS_FOCUS) ? C_FG : C_DIM)));
        text_cf(dc, r.left + w / 2, text_y_centred(r.top + h / 2, 2), w - 8, t, 2, off ? C_RULE : (active ? C_LIT : C_FG));
    }
    void draw_list_item(const DRAWITEMSTRUCT *di, bool library)
    {
        HDC dc = di->hDC;
        const RECT &r = di->rcItem;
        const bool sel = (di->itemState & ODS_SELECTED) != 0;
        fill(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, sel ? C_SEL : C_BASE);
        if (sel) fill(dc, r.left, r.top, 3, r.bottom - r.top, C_FG);
        if (di->itemID == (UINT)-1) return;
        std::wstring s;
        if (library) { if (di->itemID < g_library.size()) s = upper(game_label(di->itemID)); }
        else
        {
            const int n = (int)SendMessageW(di->hwndItem, LB_GETTEXTLEN, di->itemID, 0);
            if (n > 0) { s.resize((size_t)n + 1); SendMessageW(di->hwndItem, LB_GETTEXT, di->itemID, (LPARAM)&s[0]); s.resize((size_t)n); }
        }
        textf(dc, r.left + 12, text_y_centred((r.top + r.bottom) / 2, 2), r.right - r.left - 20, s, 2, sel ? C_FG : C_DIM);
    }

    // ---------------- window ----------------
    // WS_CLIPSIBLINGS (0.3.0): kept from the display chooser (gone in the SCAN
    // round); harmless, no control overlaps another
    HWND mk(const wchar_t *cls, const wchar_t *txt, DWORD style, int x, int y, int w, int h, int id)
    {
        return CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style, x, y, w, h, g_main, (HMENU)(INT_PTR)id, g_hinst, nullptr);
    }
    void theme_dark(HWND h, const wchar_t *sub)
    {
        typedef HRESULT (WINAPI *pfn_swt)(HWND, LPCWSTR, LPCWSTR);
        static pfn_swt p = nullptr;
        if (p == nullptr)
        {
            HMODULE m = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (m) p = (pfn_swt)(void *)GetProcAddress(m, "SetWindowTheme");
        }
        if (p) p(h, sub, nullptr);
    }
    void dark_titlebar(HWND h)
    {
        typedef HRESULT (WINAPI *pfn_dwm)(HWND, DWORD, LPCVOID, DWORD);
        HMODULE m = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m) return;
        pfn_dwm p = (pfn_dwm)(void *)GetProcAddress(m, "DwmSetWindowAttribute");
        if (!p) return;
        BOOL on = TRUE;
        if (FAILED(p(h, 20, &on, sizeof on))) p(h, 19, &on, sizeof on);
    }

    // ---------------- layout (0.3.0) ----------------
    //
    // Every button is sized to its labels in the current language - all the
    // states it shows - so no label is cut; run at start and on a language
    // change (relayout_all).
    int btn_w(std::initializer_list<const wchar_t *> labels, int minw, int pad = 24)
    {
        int w = minw;
        for (const wchar_t *s : labels) { const int t = text_w(s, 2) + pad; if (t > w) w = t; }
        return w;
    }
    // tabs, right-aligned at the page edge: INSTALL, STATUS, OPTIONS
    void relayout_tabs()
    {
        int x = PX + PW;
        for (HWND h : { g_tab_adv, g_tab_status, g_tab_guide })
        {
            if (!h) continue;
            wchar_t lab[64] = {}; GetWindowTextW(h, lab, 64);
            const int w = btn_w({ lab }, 110);
            x -= w;
            SetWindowPos(h, nullptr, x, 20, w, 24, SWP_NOZORDER | SWP_NOACTIVATE);
            g_tabs_x = x;
            x -= 10;
        }
    }
    // the status rows' buttons, right to left from the page edge, 10 apart
    void relayout_status()
    {
        int left[R_N];
        for (int r = 0; r < R_N; ++r) { left[r] = PX + PW + 10; g_row_end[r] = PX + PW; }
        auto put = [&](int r, HWND h, int w)
        {
            left[r] -= 10 + w;
            if (h) SetWindowPos(h, nullptr, left[r], st_btn_y(r), w, 24, SWP_NOZORDER | SWP_NOACTIVATE);
            g_row_end[r] = left[r] - 10;
        };
        put(R_INSTALL, g_btn_logs, btn_w({ L"LOGS" }, 110));
        put(R_INSTALL, g_btn_install, btn_w({ L"COPY FILES" }, 110));
        put(R_DISPLAY, g_btn_change, btn_w({ L"SCAN DISPLAY" }, 110));
        put(R_DIAG, g_btn_passes, btn_w({ L"1 PASS", L"2 PASSES" }, 110));
        put(R_DIAG, g_btn_res, btn_w({ k_res_label[0], k_res_label[1], k_res_label[2], k_res_label[3] }, 110));
        put(R_FIT, g_btn_fit, btn_w({ L"FORCE FIT: ON", L"IF OUTPUT RESIZE IS NOT WORKING: FORCE FIT" }, 110));
        put(R_MULTI, g_btn_multi, btn_w({ L"MULTI-DISPLAY: ON", L"MULTI-DISPLAY: OFF" }, 110));
        put(R_RECORD, g_btn_record, btn_w({ L"TURN ON", L"TURN OFF" }, 110));
        // the game-name row (y 56, STATUS and OPTIONS): RENAME, SAVE
        {
            const int sw = btn_w({ L"SAVE" }, 110), rw = btn_w({ L"RENAME" }, 110);
            if (g_btn_save) SetWindowPos(g_btn_save, nullptr, PX + PW - sw, 56, sw, 24, SWP_NOZORDER | SWP_NOACTIVATE);
            if (g_btn_rename) SetWindowPos(g_btn_rename, nullptr, PX + PW - sw - 10 - rw, 56, rw, 24, SWP_NOZORDER | SWP_NOACTIVATE);
            g_name_btn_x = PX + PW - sw - 10 - rw;
        }
    }
    // + ADD GAME and REMOVE share the library's width
    void relayout_library()
    {
        const int rw = btn_w({ L"REMOVE" }, 90, 16), aw = LW - 10 - rw;
        if (g_btn_add) SetWindowPos(g_btn_add, nullptr, LX, Y_SWEEP - 40, aw, 28, SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_btn_remove) SetWindowPos(g_btn_remove, nullptr, LX + aw + 10, Y_SWEEP - 40, rw, 28, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    // the options tab: every row's buttons flowed from ADV_VX after the row's
    // value; a button that would pass the page edge starts a second line and
    // the rows below move down. After a value the buttons move as a group (all
    // on the value's line, or all from the next). The - / + pairs keep their
    // size (40): a number's - / + sit at ADV_VX + 90 and + 138, as in 0.5.0.
    void relayout_adv()
    {
        const int right = PX + PW;
        auto bwid = [&](HWND b)
        {
            wchar_t lab[64] = {}; GetWindowTextW(b, lab, 64);
            const bool pm = (wcscmp(lab, L"-") == 0 || wcscmp(lab, L"+") == 0);
            const int bw = pm ? 40 : text_w(lab, 2) + 24;
            return (bw > right - ADV_VX) ? right - ADV_VX : bw;
        };
        int y = ADV_Y0;
        size_t c = 0;
        for (int i = 0; i < ADV_N; ++i)
        {
            const adv_row_def &d = k_adv_rows[i];
            g_adv_y[i] = y;
            g_adv_vw[i] = 0;
            if (d.val == AV_NUM) g_adv_vw[i] = 80;
            else if (d.val == AV_TEXT)
            {
                const int w1 = text_w(k_tuning_on, 2), w2 = text_w(k_tuning_off, 2);
                g_adv_vw[i] = (w1 > w2) ? w1 : w2;
                if (g_adv_vw[i] > right - ADV_VX) g_adv_vw[i] = right - ADV_VX;
            }
            int x = ADV_VX + (g_adv_vw[i] > 0 ? g_adv_vw[i] + 10 : 0), ly = y;
            if (g_adv_vw[i] > 0)
            {
                int gw = 0;
                for (int k = 0; k < d.ctl && c + (size_t)k < g_adv_ctls.size(); ++k) gw += (k ? 8 : 0) + bwid(g_adv_ctls[c + (size_t)k]);
                if (x + gw > right) { x = ADV_VX; ly += ADV_ROW; }
            }
            for (int k = 0; k < d.ctl && c < g_adv_ctls.size(); ++k, ++c)
            {
                HWND b = g_adv_ctls[c];
                const int bw = bwid(b);
                if (x + bw > right && x > ADV_VX) { x = ADV_VX; ly += ADV_ROW; }
                SetWindowPos(b, nullptr, x, ly, bw, 24, SWP_NOZORDER | SWP_NOACTIVATE);
                x += bw + 8;
            }
            y = ly + ADV_ROW;
        }
        g_adv_end = y;
    }
    void relayout_all()
    {
        relayout_tabs();
        relayout_status();
        relayout_library();
        if (g_btn_g_reshade) SetWindowPos(g_btn_g_reshade, nullptr, 0, 0, btn_w({ L"OPEN RESHADE DOWNLOAD" }, 260), 24, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_btn_g_rhi) SetWindowPos(g_btn_g_rhi, nullptr, 0, 0, btn_w({ L"OPEN RHI" }, 160), 24, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        relayout_adv();
    }

    void build_ui()
    {
        g_list = mk(L"LISTBOX", L"", WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS, LX, 48, LW, Y_SWEEP - 48 - 86, ID_LIST);
        // the language flags, one row under the library (the name is the button text, not drawn)
        for (int i = 0; i < LANG_N; ++i)
            mk(L"BUTTON", k_lang_name[i], BS_OWNERDRAW | WS_TABSTOP, LX + i * 46, Y_SWEEP - 76, 40, 26, ID_LANG_0 + i);
        theme_dark(g_list, L"DarkMode_Explorer");
        for (const auto &g : g_library) SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)g.c_str());
        g_btn_add = mk(L"BUTTON", L"+ ADD GAME", BS_OWNERDRAW | WS_TABSTOP, LX, Y_SWEEP - 40, 150, 28, ID_ADD);
        g_btn_remove = mk(L"BUTTON", L"REMOVE", BS_OWNERDRAW | WS_TABSTOP, LX + 160, Y_SWEEP - 40, 110, 28, ID_REMOVE);

        // the status rows' buttons, in tab order; relayout_status sizes them
        // and puts each on its row
        g_btn_install = mk(L"BUTTON", L"COPY FILES", BS_OWNERDRAW | WS_TABSTOP, PX, st_btn_y(R_INSTALL), 110, 24, ID_INSTALL);
        g_btn_logs = mk(L"BUTTON", L"LOGS", BS_OWNERDRAW | WS_TABSTOP, PX, st_btn_y(R_INSTALL), 110, 24, ID_OPENLOGS);
        g_btn_change = mk(L"BUTTON", L"SCAN DISPLAY", BS_OWNERDRAW | WS_TABSTOP, PX, st_btn_y(R_DISPLAY), 110, 24, ID_DISPLAY_CHANGE);
        g_btn_res = mk(L"BUTTON", k_res_label[g_res_idx], BS_OWNERDRAW | WS_TABSTOP, PX, st_btn_y(R_DIAG), 110, 24, ID_DIAG_RES);
        g_btn_passes = mk(L"BUTTON", L"1 PASS", BS_OWNERDRAW | WS_TABSTOP, PX, st_btn_y(R_DIAG), 110, 24, ID_DIAG_PASSES);
        g_btn_fit = mk(L"BUTTON", L"IF OUTPUT RESIZE IS NOT WORKING: FORCE FIT", BS_OWNERDRAW | WS_TABSTOP, PX, st_btn_y(R_FIT), 110, 24, ID_FIT_TOGGLE);
        g_btn_multi = mk(L"BUTTON", L"TURN ON", BS_OWNERDRAW | WS_TABSTOP, PX, st_btn_y(R_MULTI), 110, 24, ID_MULTI_TOGGLE);
        g_btn_record = mk(L"BUTTON", L"TURN ON", BS_OWNERDRAW | WS_TABSTOP, PX, st_btn_y(R_RECORD), 110, 24, ID_RECORD_TOGGLE);

        // tabs, left to right INSTALL (the guide), STATUS, OPTIONS - relayout_tabs places them
        g_tab_guide = mk(L"BUTTON", L"INSTALL", BS_OWNERDRAW | WS_TABSTOP, PX + PW - 350, 20, 110, 24, ID_TAB_GUIDE);
        g_tab_status = mk(L"BUTTON", L"STATUS", BS_OWNERDRAW | WS_TABSTOP, PX + PW - 230, 20, 110, 24, ID_TAB_STATUS);
        g_tab_adv = mk(L"BUTTON", L"OPTIONS", BS_OWNERDRAW | WS_TABSTOP, PX + PW - 110, 20, 110, 24, ID_TAB_ADV);
        // the guide's two links; placed at the y paint_guide gives them (set_tab)
        g_btn_g_reshade = mk(L"BUTTON", L"OPEN RESHADE DOWNLOAD", BS_OWNERDRAW | WS_TABSTOP, PX + 40, GUIDE_Y0, 260, 24, ID_GUIDE_RESHADE);
        g_btn_g_rhi = mk(L"BUTTON", L"OPEN RHI", BS_OWNERDRAW | WS_TABSTOP, PX + 40, GUIDE_Y0, 160, 24, ID_GUIDE_RHI);
        g_btn_rename = mk(L"BUTTON", L"RENAME", BS_OWNERDRAW | WS_TABSTOP, PX, 56, 110, 24, ID_RENAME);
        g_btn_save = mk(L"BUTTON", L"SAVE", BS_OWNERDRAW | WS_TABSTOP, PX, 56, 110, 24, ID_SAVE);

        // the options tab: one row per setting (k_adv_rows), buttons flowed by relayout_adv
        {
            int ay = ADV_Y0;
            auto grp = [&](std::initializer_list<std::pair<const wchar_t *, int>> items, int x0)
            {
                int x = x0;
                for (const auto &it : items)
                {
                    const int w = text_w(it.first, 2) + 24;
                    g_adv_ctls.push_back(mk(L"BUTTON", it.first, BS_OWNERDRAW | WS_TABSTOP, x, ay, w, 24, it.second));
                    x += w + 8;
                }
                ay += ADV_ROW;
            };
            auto pm = [&](int id_m, int id_p)
            {
                g_adv_ctls.push_back(mk(L"BUTTON", L"-", BS_OWNERDRAW | WS_TABSTOP, ADV_VX + 90, ay, 40, 24, id_m));
                g_adv_ctls.push_back(mk(L"BUTTON", L"+", BS_OWNERDRAW | WS_TABSTOP, ADV_VX + 138, ay, 40, 24, id_p));
                ay += ADV_ROW;
            };
            grp({ { L"OFF", ID_ADV_TUNING_OFF }, { L"ON", ID_ADV_TUNING_ON } }, ADV_VX + 320);
            grp({ { L"A", ID_ADV_STYLE_A }, { L"B", ID_ADV_STYLE_B }, { L"C", ID_ADV_STYLE_C } }, ADV_VX);
            pm(ID_ADV_TONE_M, ID_ADV_TONE_P);
            pm(ID_ADV_STRUCT_M, ID_ADV_STRUCT_P);
            pm(ID_ADV_SKIN_M, ID_ADV_SKIN_P);
            grp({ { L"OFF", ID_ADV_MASK_OFF }, { L"ON", ID_ADV_MASK_ON } }, ADV_VX);
            pm(ID_ADV_INT_M, ID_ADV_INT_P);
            grp({ { L"1", ID_ADV_PASSES_1 }, { L"2", ID_ADV_PASSES_2 } }, ADV_VX);
            // R356
            grp({ { L"LIGHT-FP8 FPS+", ID_ADV_P1N_LIGHT }, { L"FULL-FP8 FPS-", ID_ADV_P1N_FULL } }, ADV_VX);
            grp({ { L"RESIDUAL NEURAL FPS+", ID_ADV_P1R_RES }, { L"NATIVE FPS-", ID_ADV_P1R_NATIVE } }, ADV_VX);
            grp({ { L"LIGHT-FP8 FPS+", ID_ADV_P2N_LIGHT }, { L"FULL-FP8 FPS-", ID_ADV_P2N_FULL } }, ADV_VX);
            grp({ { L"RESIDUAL NEURAL FPS+", ID_ADV_P2R_RES }, { L"NATIVE FPS-", ID_ADV_P2R_NATIVE } }, ADV_VX);
            grp({ { L"OFF", ID_ADV_FG1_OFF }, { L"ON", ID_ADV_FG1_ON } }, ADV_VX);
            grp({ { L"BEST QUALITY-PERFORMANCE", ID_ADV_BEST } }, ADV_VX);
            grp({ { L"OFF", ID_ADV_FG_OFF }, { L"MAP ONLY", ID_ADV_FG_MAP }, { L"FIX VECTORS", ID_ADV_FG_FIX }, { L"FIX + THIN", ID_ADV_FG_THIN } }, ADV_VX);
            pm(ID_ADV_KEEP_M, ID_ADV_KEEP_P);
            grp({ { L"OFF", ID_ADV_NRS_OFF }, { L"PERFORMANCE", ID_ADV_NRS_ON } }, ADV_VX);   // R358: OPTIMIZATION = our NRS pass
            grp({ { L"OFF", ID_ADV_SR_OFF }, { L"NATIVE UPSCALING", ID_ADV_SR_NATIVE }, { L"EXPERIMENTAL", ID_ADV_SR_EXP } }, ADV_VX);   // R358: DLSS ON GPU 1 (OLD)
            grp({ { L"QUALITY", ID_ADV_SRQ_Q }, { L"BALANCED", ID_ADV_SRQ_B }, { L"PERFORMANCE", ID_ADV_SRQ_P } }, ADV_VX);
            grp({ { L"TITLE DEFAULT", ID_ADV_PRESET_0 }, { L"K", ID_ADV_PRESET_K }, { L"L", ID_ADV_PRESET_L }, { L"M", ID_ADV_PRESET_M } }, ADV_VX);
            grp({ { L"OFF", ID_ADV_CROP_OFF }, { L"ON", ID_ADV_CROP_ON }, { L"AUTO CROP", ID_ADV_CROP_AUTO } }, ADV_VX);
            grp({ { L"OFF", ID_ADV_N16_OFF }, { L"ON", ID_ADV_N16_ON } }, ADV_VX);
            pm(ID_ADV_N16P_M, ID_ADV_N16P_P);
        }

        fill_displays();
        res_follow_pc(0, 0, L"");   // no game yet: the primary display's mode
        SetWindowTextW(g_btn_res, k_res_label[g_res_idx]);
        relayout_all();
        if (!g_library.empty()) SendMessageW(g_list, LB_SETCURSEL, 0, 0);
        set_tab(0);
        refresh_page();
    }

    void set_tab(int t)
    {
        g_tab = t;
        const int sw_status = (t == 0) ? SW_SHOW : SW_HIDE, sw_adv = (t == 1) ? SW_SHOW : SW_HIDE,
                  sw_guide = (t == 2) ? SW_SHOW : SW_HIDE;
        for (HWND h : { g_btn_install, g_btn_logs, g_btn_change, g_btn_res, g_btn_passes, g_btn_multi, g_btn_record, g_btn_fit })
            if (h) ShowWindow(h, sw_status);
        for (HWND h : g_adv_ctls) ShowWindow(h, sw_adv);
        if (t == 2)
        {   // the y of each link is where its step's text ends: paint once off-screen to learn it
            HDC sdc = GetDC(g_main);
            HDC mem = CreateCompatibleDC(sdc);
            HBITMAP bmp = CreateCompatibleBitmap(sdc, WIN_W, WIN_H);
            HGDIOBJ ob = SelectObject(mem, bmp);
            paint_guide(mem);
            SelectObject(mem, ob); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(g_main, sdc);
            if (g_btn_g_reshade) SetWindowPos(g_btn_g_reshade, nullptr, PX + 40, g_guide_y_reshade, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            if (g_btn_g_rhi) SetWindowPos(g_btn_g_rhi, nullptr, PX + 40, g_guide_y_rhi, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
        for (HWND h : { g_btn_g_reshade, g_btn_g_rhi }) if (h) ShowWindow(h, sw_guide);
        for (HWND h : { g_btn_save, g_btn_rename }) if (h) ShowWindow(h, (t == 0 || t == 1) ? SW_SHOW : SW_HIDE);
        InvalidateRect(g_main, nullptr, TRUE);
        for (HWND h : { g_tab_status, g_tab_adv, g_tab_guide }) if (h) InvalidateRect(h, nullptr, TRUE);
    }

    void refresh_adv()
    {
        const adv_view &a = g_adv;
        const bool have = g_view.have_game;
        g_active_ids.clear();
        g_active_ids.push_back(g_tab == 0 ? ID_TAB_STATUS : g_tab == 1 ? ID_TAB_ADV : ID_TAB_GUIDE);
        if (work_dirty()) g_active_ids.push_back(ID_SAVE);   // lit while there is something to save
        if (have)
        {
            g_active_ids.push_back(a.tuning ? ID_ADV_TUNING_ON : ID_ADV_TUNING_OFF);
            g_active_ids.push_back(a.style == 0 ? ID_ADV_STYLE_A : a.style == 1 ? ID_ADV_STYLE_B : ID_ADV_STYLE_C);
            g_active_ids.push_back(a.mask ? ID_ADV_MASK_ON : ID_ADV_MASK_OFF);
            g_active_ids.push_back(a.passes == 2 ? ID_ADV_PASSES_2 : ID_ADV_PASSES_1);
            g_active_ids.push_back(a.net1 ? ID_ADV_P1N_FULL : ID_ADV_P1N_LIGHT);   // R356
            g_active_ids.push_back(a.res1 ? ID_ADV_P1R_RES : ID_ADV_P1R_NATIVE);
            g_active_ids.push_back(a.net2 ? ID_ADV_P2N_FULL : ID_ADV_P2N_LIGHT);
            g_active_ids.push_back(a.res2 ? ID_ADV_P2R_RES : ID_ADV_P2R_NATIVE);
            g_active_ids.push_back(a.fg1 ? ID_ADV_FG1_ON : ID_ADV_FG1_OFF);
            g_active_ids.push_back(a.fg == 0 ? ID_ADV_FG_OFF : a.fg == 1 ? ID_ADV_FG_MAP : a.fg == 2 ? ID_ADV_FG_FIX : ID_ADV_FG_THIN);
            // R329: one choice in OPTIMIZATION - NRS, an upscaling option, or off
            g_active_ids.push_back(a.nrs ? ID_ADV_NRS_ON : ID_ADV_NRS_OFF);                                   // R358: OPTIMIZATION
            g_active_ids.push_back(!a.sr ? ID_ADV_SR_OFF : a.sr_native ? ID_ADV_SR_NATIVE : ID_ADV_SR_EXP);    // R358: DLSS ON GPU 1 (OLD)
            g_active_ids.push_back(a.sr_mode == 2 ? ID_ADV_SRQ_Q : a.sr_mode == 1 ? ID_ADV_SRQ_B : ID_ADV_SRQ_P);
            g_active_ids.push_back(a.sr_preset == 11 ? ID_ADV_PRESET_K : a.sr_preset == 12 ? ID_ADV_PRESET_L : a.sr_preset == 13 ? ID_ADV_PRESET_M : ID_ADV_PRESET_0);
            g_active_ids.push_back(a.crop == 0 ? ID_ADV_CROP_OFF : a.crop == 1 ? ID_ADV_CROP_ON : ID_ADV_CROP_AUTO);
            g_active_ids.push_back(a.n16 ? ID_ADV_N16_ON : ID_ADV_N16_OFF);
        }
        for (HWND h : g_adv_ctls)
        {
            EnableWindow(h, have);
            InvalidateRect(h, nullptr, TRUE);
        }
        for (HWND h : { g_tab_status, g_tab_adv, g_tab_guide }) if (h) InvalidateRect(h, nullptr, TRUE);
        if (g_tab == 1) InvalidateRect(g_main, nullptr, TRUE);
        if (g_btn_save) InvalidateRect(g_btn_save, nullptr, TRUE);
    }

    BOOL CALLBACK inval_child(HWND h, LPARAM) { InvalidateRect(h, nullptr, TRUE); return TRUE; }
    void set_lang(int l)
    {
        if (l < 0 || l >= LANG_N) return;
        g_lang = l;
        save_config();
        relayout_all();
        fill_displays();
        set_tab(g_tab);
        refresh_page();
        refresh_adv();
        InvalidateRect(g_main, nullptr, TRUE);
        EnumChildWindows(g_main, inval_child, 0);
    }

    LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        switch (m)
        {
        case WM_COMMAND:
            switch (LOWORD(w))
            {
            case ID_LIST:
                if (HIWORD(w) == LBN_SELCHANGE)
                {
                    if (!confirm_leave())
                    {   // stay on the game with the unsaved changes
                        for (size_t i = 0; i < g_library.size(); ++i)
                            if (_wcsicmp(g_library[i].c_str(), g_work_game.c_str()) == 0) SendMessageW(g_list, LB_SETCURSEL, (WPARAM)i, 0);
                        break;
                    }
                    refresh_page();
                }
                break;
            case ID_SAVE: do_save(); break;
            case ID_RENAME: do_rename(); break;
            case ID_ADD: do_add_game(); break;
            case ID_REMOVE: do_remove_game(); break;
            case ID_INSTALL: do_install(); break;
            case ID_OPENLOGS: do_open_logs(); break;
            case ID_DISPLAY_CHANGE:   // SCAN DISPLAY: read displays and cards again, then the test
                fill_displays(); refresh_page(); do_diagnose();
                break;
            case ID_DIAG_RES:
                g_res_user = true;
                g_res_idx = (g_res_idx + 1) % 4;
                SetWindowTextW(g_btn_res, k_res_label[g_res_idx]); InvalidateRect(g_btn_res, nullptr, TRUE);
                break;
            case ID_DIAG_PASSES:
                g_passes = (g_passes == 1) ? 2 : 1;
                SetWindowTextW(g_btn_passes, g_passes == 1 ? L"1 PASS" : L"2 PASSES"); InvalidateRect(g_btn_passes, nullptr, TRUE);
                break;
            case ID_MULTI_TOGGLE: do_multi_toggle(); break;
            case ID_RECORD_TOGGLE: do_record_toggle(); break;
            case ID_FIT_TOGGLE: do_fit_toggle(); break;
            case ID_TAB_STATUS: set_tab(0); refresh_adv(); break;
            case ID_TAB_ADV: set_tab(1); refresh_adv(); break;
            case ID_TAB_GUIDE: set_tab(2); refresh_adv(); break;
            case ID_GUIDE_RESHADE: open_url(k_url_reshade); break;
            case ID_GUIDE_RHI: open_url(k_url_rhi); break;
            default:
                if (LOWORD(w) >= ID_LANG_0 && LOWORD(w) <= ID_LANG_LAST && HIWORD(w) == BN_CLICKED) set_lang((int)LOWORD(w) - ID_LANG_0);
                else if (LOWORD(w) >= ID_ADV_TUNING_OFF && LOWORD(w) <= ID_ADV_N16P_P && HIWORD(w) == BN_CLICKED) adv_click((int)LOWORD(w));
                break;
            }
            return 0;
        case WM_TIMER:
            if (w == TIMER_SWEEP)
            {
                ++g_frame;
                RECT r{ 0, Y_SWEEP, WIN_W, Y_SWEEP + 3 };
                InvalidateRect(h, &r, FALSE);
            }
            return 0;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT cr; GetClientRect(h, &cr);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
            HGDIOBJ ob = SelectObject(mem, bmp);
            if (ps.rcPaint.top >= Y_SWEEP) { fill(mem, 0, Y_SWEEP, cr.right, cr.bottom - Y_SWEEP, C_BASE); paint_sweep(mem); }
            else paint_page(mem);
            BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
                   mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
            SelectObject(mem, ob); DeleteObject(bmp); DeleteDC(mem);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_MEASUREITEM:
            if (w == ID_LIST) { ((MEASUREITEMSTRUCT *)l)->itemHeight = 30; return TRUE; }
            break;
        case WM_DRAWITEM:
        {
            const DRAWITEMSTRUCT *di = (const DRAWITEMSTRUCT *)l;
            if (di->CtlType == ODT_LISTBOX) { draw_list_item(di, di->CtlID == ID_LIST); return TRUE; }
            if (di->CtlType == ODT_BUTTON) { draw_button(di); return TRUE; }
            break;
        }
        case WM_CTLCOLORLISTBOX:
            SetBkColor((HDC)w, C_BASE);
            return (LRESULT)g_br_base;
        case WM_CLOSE: if (confirm_leave()) DestroyWindow(h); return 0;
        case WM_DESTROY: KillTimer(h, TIMER_SWEEP); PostQuitMessage(0); return 0;
        default:
            break;
        }
        return DefWindowProcW(h, m, w, l);
    }
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int)
{
    g_hinst = hinst;
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{ sizeof icc, ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    wchar_t exe[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    g_exe_dir = dirname(exe);
    PWSTR la = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &la)))
    { g_cfg_dir = std::wstring(la) + L"\\MGPU Bridge"; CoTaskMemFree(la); }
    else g_cfg_dir = g_exe_dir;
    g_cfg_path = g_cfg_dir + L"\\launcher.ini";
    load_config();

    g_br_base = CreateSolidBrush(C_BASE);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc; wc.hInstance = hinst; wc.lpszClassName = L"MGPUBridgeLauncher";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = g_br_base;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);
    RECT r{ 0, 0, WIN_W, WIN_H };
    const DWORD ws = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    AdjustWindowRect(&r, ws, FALSE);
    g_main = CreateWindowExW(0, wc.lpszClassName, L"MGPU Bridge Launcher 0.4.0", ws,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, hinst, nullptr);
    dark_titlebar(g_main);
    {   // the whole window on the screen it opens on: CW_USEDEFAULT can place it
        // so the bottom of the page sits under the taskbar (0.3.0)
        RECT wr{}; GetWindowRect(g_main, &wr);
        MONITORINFO mi{}; mi.cbSize = sizeof mi;
        if (GetMonitorInfoW(MonitorFromWindow(g_main, MONITOR_DEFAULTTONEAREST), &mi))
        {
            const RECT &wa = mi.rcWork;
            const int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
            int x = wr.left, y = wr.top;
            if (x + ww > wa.right) x = wa.right - ww;
            if (y + wh > wa.bottom) y = wa.bottom - wh;
            if (x < wa.left) x = wa.left;
            if (y < wa.top) y = wa.top;
            if (x != wr.left || y != wr.top) SetWindowPos(g_main, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    build_ui();
    SetTimer(g_main, TIMER_SWEEP, 33, nullptr);
    ShowWindow(g_main, SW_SHOW);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(g_main, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    CoUninitialize();
    return 0;
}
