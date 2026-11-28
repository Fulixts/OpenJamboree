/* OpenJamboree in-game menu: Esc (or View+Menu on an Xbox pad) opens it over the game.
   Dear ImGui (MIT, runtime/imgui) drawn with its OpenGL 3 backend on the GL thread, right before SwapBuffers
   (backend_win32.c: backend_gl_swap). The window thread and the game thread only push input events into a small
   locked queue; everything ImGui runs on the GL thread. Settings go through ini_set_apply (main.c), the same path as
   editing the .ini, so the menu and the file always agree.
   While the menu is open the game gets no keyboard, mouse or pad input (backend_win32.c: UI_BLOCK). */
#include "imgui/imgui.h"
#include "imgui/imgui_impl_opengl3.h"
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
const char* ini_value(const char* k, const char* def);
void ini_set_apply(const char* k, const char* v);
void backend_window_size(int* w, int* h);
void backend_request_fullscreen(int on);
int backend_is_fullscreen(void);
void backend_request_quit(void);
int lan_menu_ifaces(char labels[][128], char ips[][20], int max);
void lan_status(char* out, int n);
const char* lan_if_name(void);
unsigned lan_ip(void);
extern char g_exename[];
}

/* ---------------------------------------------------------------- input queue (any thread -> GL thread) */
enum { EV_KEY, EV_CHAR, EV_MOUSE_POS, EV_MOUSE_BTN, EV_WHEEL, EV_PAD };
struct Ev { int type, a, b; float f; };
static Ev g_q[256]; static int g_qn; static SRWLOCK g_ql = SRWLOCK_INIT;
static volatile LONG g_open, g_closing_esc;

static void push(int type, int a, int b, float f) {
  AcquireSRWLockExclusive(&g_ql);
  if (g_qn < 256) { g_q[g_qn].type = type; g_q[g_qn].a = a; g_q[g_qn].b = b; g_q[g_qn].f = f; g_qn++; }
  ReleaseSRWLockExclusive(&g_ql);
}

static ImGuiKey vk_to_key(int vk) {
  switch (vk) {
    case VK_TAB: return ImGuiKey_Tab; case VK_LEFT: return ImGuiKey_LeftArrow; case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_UP: return ImGuiKey_UpArrow; case VK_DOWN: return ImGuiKey_DownArrow; case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown; case VK_HOME: return ImGuiKey_Home; case VK_END: return ImGuiKey_End;
    case VK_DELETE: return ImGuiKey_Delete; case VK_BACK: return ImGuiKey_Backspace; case VK_SPACE: return ImGuiKey_Space;
    case VK_RETURN: return ImGuiKey_Enter; case VK_ESCAPE: return ImGuiKey_Escape;
    case VK_SHIFT: return ImGuiKey_LeftShift; case VK_CONTROL: return ImGuiKey_LeftCtrl; case VK_MENU: return ImGuiKey_LeftAlt;
  }
  return ImGuiKey_None;
}

extern "C" int menu_is_open(void) { return g_open != 0; }
extern "C" void menu_toggle(void) { InterlockedExchange(&g_open, !g_open); }
/* window thread: keys while the menu is open (Esc closes it) */
extern "C" void menu_key(int vk, int down) {
  if (vk == VK_ESCAPE) { if (down) menu_toggle(); return; }
  push(EV_KEY, vk, down, 0);
}
extern "C" void menu_char(unsigned c) { push(EV_CHAR, (int)c, 0, 0); }
extern "C" void menu_mouse_pos(int x, int y) { push(EV_MOUSE_POS, x, y, 0); }
extern "C" void menu_mouse_button(int b, int down) { push(EV_MOUSE_BTN, b, down, 0); }
extern "C" void menu_wheel(float d) { push(EV_WHEEL, 0, 0, d); }

/* game thread (video_poll): XInput buttons of the first pad, raw xinput bits. View+Menu together toggles the menu */
extern "C" void menu_pad(unsigned buttons, int connected) {
  static unsigned prev; static int combo;
  if (!connected) { prev = 0; return; }
  int both = (buttons & 0x0030) == 0x0030;
  if (both && !combo) menu_toggle();
  combo = both;
  if (g_open && buttons != prev) push(EV_PAD, (int)buttons, (int)prev, 0);
  prev = buttons;
}

/* ---------------------------------------------------------------- settings helpers */
static int iv(const char* k, int def) { char d[16]; snprintf(d, sizeof d, "%d", def); return atoi(ini_value(k, d)); }
static float fv(const char* k, float def) { char d[16]; snprintf(d, sizeof d, "%g", def); return (float)atof(ini_value(k, d)); }
static void seti(const char* k, int v) { char s[16]; snprintf(s, sizeof s, "%d", v); ini_set_apply(k, s); }
static void setf(const char* k, float v) { char s[16]; snprintf(s, sizeof s, "%.2f", v); ini_set_apply(k, s); }

static bool toggle(const char* label, const char* key, int def, const char* help) {
  bool on = iv(key, def) != 0;
  if (ImGui::Checkbox(label, &on)) { seti(key, on ? 1 : 0); }
  if (help && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help);
  return on;
}

static void combo(const char* label, const char* key, const char* const* names, const char* const* values, int n, const char* def) {
  const char* cur = ini_value(key, def); int sel = 0;
  for (int i = 0; i < n; i++) if (!strcmp(cur, values[i]) || atof(cur) == atof(values[i]) && values[i][0] >= '0' && values[i][0] <= '9') { sel = i; break; }
  if (ImGui::BeginCombo(label, names[sel])) {
    for (int i = 0; i < n; i++) { bool s = i == sel; if (ImGui::Selectable(names[i], s)) ini_set_apply(key, values[i]); if (s) ImGui::SetItemDefaultFocus(); }
    ImGui::EndCombo();
  }
}

/* ---------------------------------------------------------------- the menu */
static bool g_ready, g_quit_ask;
static float g_scale = 1;

static void setup(int h) {
  IMGUI_CHECKVERSION(); ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = NULL; io.LogFilename = NULL;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  g_scale = h >= 2000 ? 2.0f : h >= 1400 ? 1.5f : h >= 1000 ? 1.25f : 1.0f;
  /* Segoe UI from Windows (Latin-1: accents for Portuguese), the built-in font if it is missing */
  char font[MAX_PATH]; GetWindowsDirectoryA(font, MAX_PATH); strcat(font, "\\Fonts\\segoeui.ttf");
  if (GetFileAttributesA(font) != INVALID_FILE_ATTRIBUTES) io.Fonts->AddFontFromFileTTF(font, 19.0f * g_scale);
  else { ImFontConfig c; c.SizePixels = 13.0f * g_scale; io.Fonts->AddFontDefault(&c); }
  ImGui::StyleColorsDark();
  ImGuiStyle& st = ImGui::GetStyle();
  st.WindowRounding = 10; st.FrameRounding = 6; st.GrabRounding = 6; st.TabRounding = 6; st.WindowPadding = ImVec2(18, 16);
  st.ItemSpacing = ImVec2(10, 9); st.FramePadding = ImVec2(10, 6);
  ImVec4* c = st.Colors;
  c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.08f, 0.12f, 0.96f);
  c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBg] = ImVec4(0.80f, 0.14f, 0.16f, 1.0f);
  c[ImGuiCol_Tab] = ImVec4(0.18f, 0.20f, 0.28f, 1); c[ImGuiCol_TabSelected] = ImVec4(0.80f, 0.14f, 0.16f, 1); c[ImGuiCol_TabHovered] = ImVec4(0.92f, 0.30f, 0.30f, 1);
  c[ImGuiCol_Button] = ImVec4(0.20f, 0.23f, 0.32f, 1); c[ImGuiCol_ButtonHovered] = ImVec4(0.92f, 0.30f, 0.30f, 1); c[ImGuiCol_ButtonActive] = ImVec4(0.70f, 0.10f, 0.12f, 1);
  c[ImGuiCol_CheckMark] = ImVec4(1.0f, 0.82f, 0.20f, 1); c[ImGuiCol_SliderGrab] = ImVec4(1.0f, 0.82f, 0.20f, 1);
  c[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.16f, 0.22f, 1); c[ImGuiCol_Header] = ImVec4(0.80f, 0.14f, 0.16f, 0.6f);
  st.ScaleAllSizes(g_scale);
  ImGui_ImplOpenGL3_Init("#version 430");
  g_ready = true;
}

static void drain(void) {
  ImGuiIO& io = ImGui::GetIO();
  AcquireSRWLockExclusive(&g_ql);
  for (int i = 0; i < g_qn; i++) {
    Ev& e = g_q[i];
    switch (e.type) {
      case EV_KEY: { ImGuiKey k = vk_to_key(e.a); if (k != ImGuiKey_None) io.AddKeyEvent(k, e.b != 0);
        if (e.a == VK_CONTROL) io.AddKeyEvent(ImGuiMod_Ctrl, e.b != 0); if (e.a == VK_SHIFT) io.AddKeyEvent(ImGuiMod_Shift, e.b != 0); break; }
      case EV_CHAR: io.AddInputCharacter((unsigned)e.a); break;
      case EV_MOUSE_POS: io.AddMousePosEvent((float)e.a, (float)e.b); break;
      case EV_MOUSE_BTN: io.AddMouseButtonEvent(e.a, e.b != 0); break;
      case EV_WHEEL: io.AddMouseWheelEvent(0, e.f); break;
      case EV_PAD: {
        static const struct { unsigned bit; ImGuiKey key; } map[] = {
          {0x0001, ImGuiKey_GamepadDpadUp}, {0x0002, ImGuiKey_GamepadDpadDown}, {0x0004, ImGuiKey_GamepadDpadLeft}, {0x0008, ImGuiKey_GamepadDpadRight},
          {0x1000, ImGuiKey_GamepadFaceDown}, {0x2000, ImGuiKey_GamepadFaceRight}, {0x4000, ImGuiKey_GamepadFaceLeft}, {0x8000, ImGuiKey_GamepadFaceUp},
          {0x0100, ImGuiKey_GamepadL1}, {0x0200, ImGuiKey_GamepadR1}};
        unsigned now = (unsigned)e.a, was = (unsigned)e.b;
        for (auto& m : map) if ((now ^ was) & m.bit) io.AddKeyEvent(m.key, (now & m.bit) != 0);
        /* B on the pad closes the menu like Esc, when nothing inside wants it */
        if ((now & 0x2000) && !(was & 0x2000) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup)) InterlockedExchange(&g_open, 0);
        break; }
    }
  }
  g_qn = 0;
  ReleaseSRWLockExclusive(&g_ql);
}

static void tab_game(void) {
  if (ImGui::Button("Continuar jogando", ImVec2(-1, 0))) InterlockedExchange(&g_open, 0);
  ImGui::Spacing();
  bool fs = backend_is_fullscreen() != 0;
  if (ImGui::Checkbox("Tela cheia", &fs)) { backend_request_fullscreen(fs ? 1 : 0); seti("fullscreen", fs ? 1 : 0); }
  toggle("Pular o vídeo de abertura", "skip_intro", 1, "Toca só 1 segundo da abertura de 3 minutos. Vale no próximo início.");
  static const char* const ln[] = {"Português (Brasil)", "English (US)", "English (UK)", "Español (Latinoamérica)", "Español", "Français", "Deutsch", "Italiano", "日本語"};
  static const char* const lv[] = {"17", "1", "12", "14", "5", "2", "3", "4", "0"};
  combo("Idioma do jogo", "language", ln, lv, 9, "1");
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Vale no próximo início.");
  ImGui::TextDisabled("Opções marcadas \"próximo início\" pedem fechar e abrir o jogo.");
}

static void tab_graphics(void) {
  ImGui::SeparatorText("Qualidade da imagem");
  static const char* const dn[] = {"Modo TV: 1080p (mais nítido)", "Modo portátil: 720p (mais leve)"};
  static const char* const dv[] = {"1", "0"};
  combo("Modo do console", "docked", dn, dv, 2, "1");
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("O jogo desenha em 1080p no modo TV e em 720p no portátil. Vale no próximo início.");
  static const char* const rn[] = {"50% (mais rápido)", "75%", "100% (nativo)", "150%", "200%", "300%", "400%"};
  static const char* const rv[] = {"0.5", "0.75", "1", "1.5", "2", "3", "4"};
  combo("Resolução interna", "resolution", rn, rv, 7, "1");
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Resolução do 3D em relação à do console. Muda na hora.");
  toggle("Resolução dinâmica", "dynamic_resolution", 0, "Baixa a resolução sozinho quando a placa de vídeo não dá conta, para manter os 60 FPS.");
  if (iv("dynamic_resolution", 0)) {
    float m = fv("dynamic_resolution_min", 0.6f) * 100;
    if (ImGui::SliderFloat("Resolução mínima", &m, 25, 100, "%.0f%%")) setf("dynamic_resolution_min", m / 100);
  }
  ImGui::TextDisabled("1080p x 150%% = 1620p, x 200%% = 4K. Placas como a RTX 3060 aguentam 150%% a 200%%.");
  toggle("Sombras", "shadows", 1, "Desligar ganha FPS em placas fracas.");
  ImGui::SeparatorText("Carregamento");
  toggle("Guardar texturas convertidas no disco", "texture_cache", 1,
         "Deixa as telas de carregamento bem mais curtas a partir da segunda vez. Vale no próximo início.");
  ImGui::SeparatorText("Fluidez");
  bool lim = iv("fps_limit", 1) == 1;
  if (ImGui::Checkbox("Travar em 60 FPS (como no console)", &lim)) seti("fps_limit", lim ? 1 : 0);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("O jogo roda em 60. Destravar não deixa o jogo mais rápido, só gasta mais a placa.");
  toggle("V-Sync forçado", "forced_vsync", 0, "Tira o tearing, mas pode derrubar o FPS em PCs lentos.");
  toggle("Compilar shaders em segundo plano", "shader_thread", 1, "Evita travadinhas quando aparece um efeito novo.");
}

static void tab_controls(void) {
  static const char* const pn[] = {"Pelo nome (A do Xbox = A)", "Pela posição (como no Switch)"};
  static const char* const pv[] = {"0", "1"};
  combo("Botões do controle", "pad_buttons", pn, pv, 2, "0");
  ImGui::SeparatorText("Teclado");
  ImGui::TextWrapped("Setas = direcional   WASD = analógico   X/Espaço = A   Z = B   C = X   V = Y\n"
                     "Q = L   E = R   1 = ZL   3 = ZR   Enter = +   Backspace = -");
  ImGui::SeparatorText("Atalhos");
  ImGui::TextWrapped("Esc = este menu (no controle: View + Menu)   F3 = captura de tela   F7 = gravar vídeo   F11 = tela cheia");
}

static void tab_network(void) {
  ImGui::TextWrapped("Jogo local sem fio pela rede: todos no mesmo LAN ou na mesma rede ZeroTier, sem servidor. "
                     "Quem cria a sala e quem entra usam o menu de jogo local do próprio jogo.");
  ImGui::SeparatorText("Rede usada");
  static char labels[16][128], ips[16][20]; static int n = -1;
  if (n < 0 || ImGui::Button("Procurar adaptadores")) n = lan_menu_ifaces(labels, ips, 16);
  const char* cur = ini_value("lan_interface", "");
  const char* shown = *cur ? cur : "Automático (ZeroTier, senão a rede com internet)";
  for (int i = 0; i < n; i++) if (!strcmp(cur, ips[i])) shown = labels[i];
  if (ImGui::BeginCombo("Adaptador", shown)) {
    if (ImGui::Selectable("Automático (ZeroTier, senão a rede com internet)", !*cur)) ini_set_apply("lan_interface", "");
    for (int i = 0; i < n; i++) if (ImGui::Selectable(labels[i], !strcmp(cur, ips[i]))) ini_set_apply("lan_interface", ips[i]);
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Vale ao abrir o jogo local de novo. Todos os PCs precisam estar na mesma sub-rede.");
  unsigned ip = lan_ip();
  ImGui::Text("Em uso: %s  %u.%u.%u.%u", lan_if_name(), ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
  ImGui::SeparatorText("PCs extras (opcional)");
  static char peers[256]; static bool loaded;
  if (!loaded) { snprintf(peers, sizeof peers, "%s", ini_value("lan_peers", "")); loaded = true; }
  if (ImGui::InputText("IPs dos outros PCs", peers, sizeof peers)) ini_set_apply("lan_peers", peers);
  ImGui::TextDisabled("Só para VPNs sem broadcast (Tailscale). Na ZeroTier normal deixe vazio.");
  ImGui::SeparatorText("Sala");
  char st[96]; lan_status(st, sizeof st);
  ImGui::Text("Estado: %s", st);
  ImGui::TextDisabled("O Windows marca a rede ZeroTier como Pública e bloqueia a porta 11452: marque-a como Privada.");
}

static void tab_about(void) {
  ImGui::TextWrapped("OpenJamboree: Super Mario Party Jamboree recompilado para PC a partir da sua própria cópia do jogo.");
  ImGui::Spacing();
  ImGui::TextWrapped("Feito com SwitchRecomp by MZ Labs (AGPL v3). Port original: drewkellar/OpenJamboree. Menu: Dear ImGui.");
  ImGui::TextWrapped("Nenhum arquivo do jogo, chave ou firmware vem junto: tudo sai do seu dump.");
}

extern "C" void menu_draw_gl(void) {
  if (!g_open && !g_ready) return;
  int w, h; backend_window_size(&w, &h);
  if (!g_ready) setup(h);
  drain();
  if (!g_open) return;
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2((float)w, (float)h);
  static LARGE_INTEGER fq, last; if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
  LARGE_INTEGER now; QueryPerformanceCounter(&now);
  io.DeltaTime = last.QuadPart ? (float)(now.QuadPart - last.QuadPart) / fq.QuadPart : 1.0f / 60; if (io.DeltaTime <= 0) io.DeltaTime = 1.0f / 60; last = now;

  ImGui_ImplOpenGL3_NewFrame();
  ImGui::NewFrame();
  /* dim the game behind the menu */
  ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 120));
  ImVec2 size(620 * g_scale, 480 * g_scale);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  ImGui::Begin("OpenJamboree", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
  if (ImGui::BeginTabBar("tabs")) {
    if (ImGui::BeginTabItem("Jogo")) { tab_game(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Gráficos")) { tab_graphics(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Controles")) { tab_controls(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Rede")) { tab_network(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Sobre")) { tab_about(); ImGui::EndTabItem(); }
    ImGui::EndTabBar();
  }
  ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetFrameHeight() - ImGui::GetStyle().WindowPadding.y);
  if (ImGui::Button("Sair do jogo")) ImGui::OpenPopup("sair");
  ImGui::SameLine(); ImGui::TextDisabled("  Esc ou B fecha o menu");
  if (ImGui::BeginPopupModal("sair", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
    ImGui::Text("Sair do jogo? O que não foi salvo no jogo se perde.");
    if (ImGui::Button("Sair")) backend_request_quit();
    ImGui::SameLine(); if (ImGui::Button("Voltar")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  ImGui::End();
  ImGui::Render();

  /* draw on the window's own framebuffer, over the finished game frame */
  typedef void (APIENTRY *BindFB)(unsigned, unsigned);
  static BindFB bind = (BindFB)wglGetProcAddress("glBindFramebuffer");
  if (bind) bind(0x8D40 /* GL_FRAMEBUFFER */, 0);
  glViewport(0, 0, w, h);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}
