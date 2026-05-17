#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <SDL.h>
#ifdef _WIN32
#include "platform/win32/volume_control.h"
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "snes/ppu.h"

#include "types.h"
#include "sm_rtl.h"
#include "sm_cpu_infra.h"
#include "config.h"
#include "util.h"
#include "spc_player.h"

#ifdef __SWITCH__
#include "switch_impl.h"
#endif

static void playAudio(Snes *snes, SDL_AudioDeviceID device, int16_t *audioBuffer);
static void renderScreen(Snes *snes, SDL_Renderer *renderer, SDL_Texture *texture);
static void SDLCALL AudioCallback(void *userdata, Uint8 *stream, int len);
static void SwitchDirectory();
static void RenderNumber(uint8 *dst, size_t pitch, int n, uint8 big);
static void OpenOneGamepad(int i);
static void HandleVolumeAdjustment(int volume_adjustment);
static void HandleGamepadAxisInput(int gamepad_id, int axis, int value);
static int RemapSdlButton(int button);
static void HandleGamepadInput(int button, bool pressed);
static void HandleInput(int keyCode, int keyMod, bool pressed);
static void HandleCommand(uint32 j, bool pressed);
void OpenGLRenderer_Create(struct RendererFuncs *funcs);

bool g_debug_flag;
bool g_is_turbo;
bool g_want_dump_memmap_flags;
bool g_new_ppu = true;
bool g_other_image;
struct SpcPlayer *g_spc_player;
static uint32_t button_state;

static uint8_t g_pixels[256 * 4 * 240];
static uint8_t g_my_pixels[256 * 4 * 240];

int g_got_mismatch_count;

enum {
  kDefaultFullscreen = 0,
  kMaxWindowScale = 10,
  kDefaultFreq = 44100,
  kDefaultChannels = 2,
  kDefaultSamples = 2048,
};

static const char kWindowTitle[] = "Super Metroid";
static uint32 g_win_flags = SDL_WINDOW_RESIZABLE;
static SDL_Window *g_window;

static uint8 g_paused, g_turbo, g_replay_turbo = true, g_cursor = true;
static uint8 g_current_window_scale;
static uint8 g_gamepad_buttons;
static int g_input1_state;
static bool g_display_perf;
static int g_curr_fps;
static int g_ppu_render_flags = 0;
static int g_snes_width, g_snes_height;
static int g_sdl_audio_mixer_volume = SDL_MIX_MAXVOLUME;
static struct RendererFuncs g_renderer_funcs;
static uint32 g_gamepad_modifiers;
static uint16 g_gamepad_last_cmd[kGamepadBtn_Count];
extern Snes *g_snes;

void NORETURN Die(const char *error) {
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kWindowTitle, error, NULL);
  fprintf(stderr, "Error: %s\n", error);
  exit(1);
}

void Warning(const char *error) {
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, kWindowTitle, error, NULL);
  fprintf(stderr, "Warning: %s\n", error);
}

void ChangeWindowScale(int scale_step) {
  if ((SDL_GetWindowFlags(g_window) & (SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED | SDL_WINDOW_MAXIMIZED)) != 0)
    return;
  int screen = SDL_GetWindowDisplayIndex(g_window);
  if (screen < 0) screen = 0;
  int max_scale = kMaxWindowScale;
  SDL_Rect bounds;
  int bt = -1, bl, bb, br;
  if (SDL_GetDisplayUsableBounds(screen, &bounds) == 0) {
    if (SDL_GetWindowBordersSize(g_window, &bt, &bl, &bb, &br) != 0) {
      bl = br = bb = 1;
      bt = 31;
    }
    int mw = (bounds.w - bl - br + g_snes_width / 4) / g_snes_width;
    int mh = (bounds.h - bt - bb + g_snes_height / 4) / g_snes_height;
    max_scale = IntMin(mw, mh);
  }
  int new_scale = IntMax(IntMin(g_current_window_scale + scale_step, max_scale), 1);
  g_current_window_scale = new_scale;
  int w = new_scale * g_snes_width;
  int h = new_scale * g_snes_height;

  SDL_SetWindowSize(g_window, w, h);
  if (bt >= 0) {
    int mx, my;
    SDL_GetGlobalMouseState(&mx, &my);
    int wx = IntMax(IntMin(mx - w / 2, bounds.x + bounds.w - bl - br - w), bounds.x + bl);
    int wy = IntMax(IntMin(my - h / 2, bounds.y + bounds.h - bt - bb - h), bounds.y + bt);
    SDL_SetWindowPosition(g_window, wx, wy);
  } else {
    SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
  }
}

#define RESIZE_BORDER 20
static SDL_HitTestResult HitTestCallback(SDL_Window *win, const SDL_Point *pt, void *data) {
  uint32 flags = SDL_GetWindowFlags(win);
  if ((flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0 || (flags & SDL_WINDOW_FULLSCREEN) != 0)
    return SDL_HITTEST_NORMAL;

  if ((SDL_GetModState() & KMOD_CTRL) != 0)
    return SDL_HITTEST_DRAGGABLE;

  int w, h;
  SDL_GetWindowSize(win, &w, &h);

  if (pt->y < RESIZE_BORDER) {
    return (pt->x < RESIZE_BORDER) ? SDL_HITTEST_RESIZE_TOPLEFT :
      (pt->x >= w - RESIZE_BORDER) ? SDL_HITTEST_RESIZE_TOPRIGHT : SDL_HITTEST_RESIZE_TOP;
  } else if (pt->y >= h - RESIZE_BORDER) {
    return (pt->x < RESIZE_BORDER) ? SDL_HITTEST_RESIZE_BOTTOMLEFT :
      (pt->x >= w - RESIZE_BORDER) ? SDL_HITTEST_RESIZE_BOTTOMRIGHT : SDL_HITTEST_RESIZE_BOTTOM;
  } else {
    if (pt->x < RESIZE_BORDER) {
      return SDL_HITTEST_RESIZE_LEFT;
    } else if (pt->x >= w - RESIZE_BORDER) {
      return SDL_HITTEST_RESIZE_RIGHT;
    }
  }
  return SDL_HITTEST_NORMAL;
}

void RtlDrawPpuFrame(uint8 *pixel_buffer, size_t pitch, uint32 render_flags) {
  uint8 *ppu_pixels = g_other_image ? g_my_pixels : g_pixels;
  for (size_t y = 0; y < 240; y++)
    memcpy((uint8_t *)pixel_buffer + y * pitch, ppu_pixels + y * 256 * 4, 256 * 4);
}

static void DrawPpuFrameWithPerf(void) {
  int render_scale = PpuGetCurrentRenderScale(g_snes->ppu, g_ppu_render_flags);
  uint8 *pixel_buffer = 0;
  int pitch = 0;

  g_renderer_funcs.BeginDraw(g_snes_width * render_scale,
                             g_snes_height * render_scale,
                             &pixel_buffer, &pitch);
  if (g_display_perf || g_config.display_perf_title) {
    static float history[64], average;
    static int history_pos;
    uint64 before = SDL_GetPerformanceCounter();
    RtlDrawPpuFrame(pixel_buffer, pitch, g_ppu_render_flags);
    uint64 after = SDL_GetPerformanceCounter();
    float v = (double)SDL_GetPerformanceFrequency() / (after - before);
    average += v - history[history_pos];
    history[history_pos] = v;
    history_pos = (history_pos + 1) & 63;
    g_curr_fps = average * (1.0f / 64);
  } else {
    RtlDrawPpuFrame(pixel_buffer, pitch, g_ppu_render_flags);
  }
  if (g_display_perf)
    RenderNumber(pixel_buffer + pitch * render_scale, pitch, g_curr_fps, render_scale == 4);

  if (g_got_mismatch_count)
    RenderNumber(pixel_buffer + pitch * render_scale, pitch, g_got_mismatch_count, render_scale == 4);

  g_renderer_funcs.EndDraw();
}

static SDL_mutex *g_audio_mutex;
static uint8 *g_audiobuffer, *g_audiobuffer_cur, *g_audiobuffer_end;
static int g_frames_per_block;
static uint8 g_audio_channels;
static SDL_AudioDeviceID g_audio_device;

void RtlApuLock(void) {
  SDL_LockMutex(g_audio_mutex);
}

void RtlApuUnlock(void) {
  SDL_UnlockMutex(g_audio_mutex);
}

static void SDLCALL AudioCallback(void *userdata, Uint8 *stream, int len) {
  if (SDL_LockMutex(g_audio_mutex)) Die("Mutex lock failed!");
  while (len != 0) {
    if (g_audiobuffer_end - g_audiobuffer_cur == 0) {
      RtlRenderAudio((int16 *)g_audiobuffer, g_frames_per_block, g_audio_channels);
      g_audiobuffer_cur = g_audiobuffer;
      g_audiobuffer_end = g_audiobuffer + g_frames_per_block * g_audio_channels * sizeof(int16);
    }
    int n = IntMin(len, g_audiobuffer_end - g_audiobuffer_cur);
    if (g_sdl_audio_mixer_volume == SDL_MIX_MAXVOLUME) {
      memcpy(stream, g_audiobuffer_cur, n);
    } else {
      SDL_memset(stream, 0, n);
      SDL_MixAudioFormat(stream, g_audiobuffer_cur, AUDIO_S16, n, g_sdl_audio_mixer_volume);
    }
    g_audiobuffer_cur += n;
    stream += n;
    len -= n;
  }
  SDL_UnlockMutex(g_audio_mutex);
}

// =============================================================================
// ANOTACIÓN: CONTROL DE MODOS DE DOBLE PANTALLA (TV + GAMEPAD)
// =============================================================================
typedef enum {
  kDualScreen_Clone = 0,   // Modo actual: Ambas pantallas muestran exactamente lo mismo
  kDualScreen_AlwaysMap   // Nueva opción para fijar el mapa en la segunda pantalla
} DualScreenMode;

// Por defecto, iniciamos en el modo clonado que ya funciona perfectamente
//static DualScreenMode g_dual_screen_mode = kDualScreen_Clone;
static DualScreenMode g_dual_screen_mode = kDualScreen_AlwaysMap;

// =============================================================================
// ANOTACIÓN: SISTEMA DE DOBLE VENTANA Y TEXTURAS INDEPENDIENTES
// =============================================================================
// Para evitar pantallas oscuras en SDL2, cada ventana (Window) requiere su propio 
// Renderizador (Renderer), y cada renderizador debe poseer su propia Textura (Texture).
// No se pueden compartir texturas entre renderizadores distintos.
// =============================================================================
static SDL_Window *g_window_tv = NULL;
static SDL_Window *g_window_gamepad = NULL;
static SDL_Renderer *g_renderer_tv = NULL;
static SDL_Renderer *g_renderer_gamepad = NULL;
static SDL_Renderer *g_renderer = NULL; // Puntero de compatibilidad con código antiguo

// ANOTACIÓN: Separamos las texturas para la TV y el GamePad de manera explícita
static SDL_Texture *g_texture_tv = NULL;
static SDL_Texture *g_texture_gamepad = NULL;
static SDL_Rect g_sdl_renderer_rect;

static bool SdlRenderer_Init(SDL_Window *window) {
  // Asignamos la ventana principal creada en el main a la TV de forma segura
  g_window_tv = window;
  
  // Creamos el renderizador acelerado por hardware para la pantalla de la TV
  g_renderer_tv = SDL_CreateRenderer(g_window_tv, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  g_renderer = g_renderer_tv; // Mantenemos la referencia para compatibilidad interna

  // Creamos la segunda ventana dedicada al GamePad (Mapa del juego)
  g_window_gamepad = SDL_CreateWindow(
      "Super Metroid Rewrite - GamePad (Mapa)",
      900, 100,          // Coordenadas de inicio en pantalla
      256 * 2, 224 * 2,  // Escalado inicial a 2x
      SDL_WINDOW_SHOWN
  );
  
  // Creamos el renderizador acelerado por hardware para la pantalla del GamePad
  g_renderer_gamepad = SDL_CreateRenderer(g_window_gamepad, -1, SDL_RENDERER_ACCELERATED);

  // Verificación de seguridad: si alguno de los dos renderizadores falla, abortamos
  if (!g_renderer_tv || !g_renderer_gamepad) {
    printf("Error: No se pudieron inicializar los renderizadores de SDL2.\n");
    return false;
  }

  // Configuración de aspectos y filtros basados en el archivo de configuración (sm.ini)
  if (!g_config.ignore_aspect_ratio)
    SDL_RenderSetLogicalSize(g_renderer_tv, g_snes_width, g_snes_height);
  if (g_config.linear_filtering)
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");

  // Calculamos el multiplicador de texturas (requerido para mejoras visuales como Mode 7 HD)
  int tex_mult = (g_ppu_render_flags & kPpuRenderFlags_4x4Mode7) ? 4 : 1;
  
  // ANOTACIÓN CRÍTICA: Creamos g_texture_tv vinculada ÚNICAMENTE a g_renderer_tv
  g_texture_tv = SDL_CreateTexture(g_renderer_tv, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                   g_snes_width * tex_mult, g_snes_height * tex_mult);
                                
  // ANOTACIÓN CRÍTICA: Creamos g_texture_gamepad vinculada ÚNICAMENTE a g_renderer_gamepad
  g_texture_gamepad = SDL_CreateTexture(g_renderer_gamepad, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                       g_snes_width * tex_mult, g_snes_height * tex_mult);

  if (g_texture_tv == NULL || g_texture_gamepad == NULL) {
    printf("Error Crítico: Falló la creación de las texturas independientes: %s\n", SDL_GetError());
    return false;
  }
  return true;
}

static void SdlRenderer_Destroy(void) {
  // Liberación ordenada de recursos de hardware para evitar fugas de memoria (Memory Leaks)
  if (g_renderer_tv) SDL_DestroyRenderer(g_renderer_tv);
  if (g_renderer_gamepad) SDL_DestroyRenderer(g_renderer_gamepad);
  if (g_window_gamepad) SDL_DestroyWindow(g_window_gamepad);
  if (g_texture_tv) SDL_DestroyTexture(g_texture_tv);
  if (g_texture_gamepad) SDL_DestroyTexture(g_texture_gamepad);
}

static void SdlRenderer_BeginDraw(int width, int height, uint8 **pixels, int *pitch) {
  g_sdl_renderer_rect.w = width;
  g_sdl_renderer_rect.h = height;
  
  // ANOTACIÓN: Bloqueamos la textura primaria de la TV para obtener acceso directo a su buffer de memoria 
  // e inyectar los píxeles generados por la emulación de la PPU de la SNES.
  if (SDL_LockTexture(g_texture_tv, &g_sdl_renderer_rect, (void **)pixels, pitch) != 0) {
    printf("Error al bloquear textura de TV: %s\n", SDL_GetError());
    return;
  }
}

// Supongamos que esta función existe o la extraemos del módulo PPU del port:
// Forza el renderizado de la pantalla de pausa/mapa de la WRAM al buffer de píxeles indicado
extern void Ppu_RenderPauseMapDirect(void *target_pixels, int pitch);
// =============================================================================
// ANOTACIÓN: SISTEMA DE CLONACIÓN SEGURO Y DEFINITIVO (MÉTODO BUFFER)
// =============================================================================
// Para que SDL2 permita dibujar en dos ventanas simultáneas sin pantallas negras,
// bloqueamos ambas texturas y transferimos los píxeles directamente. 
// Esto garantiza que el juego, los menús de pausa y las pestañas se vean en ambos lados.
// =============================================================================
static void SdlRenderer_EndDraw(void) {
  // 1. Desbloqueamos la textura de la TV para consolidar los píxeles de la PPU (contiene el gameplay normal)
  SDL_UnlockTexture(g_texture_tv);

  // 2. CONTROL DE FLUJO SEGÚN EL MODO SELECCIONADO
  // LOGICA DE SEPARACIÓN
  if (g_dual_screen_mode == kDualScreen_Clone) {
    // =============================================================================
    // ANOTACIÓN: MODO CLONADO PERMANENTE (Ideal para Off-TV Play en Wii U)
    // =============================================================================
  // Espejo idéntico (lo que ya teníamos)
    void *tv_pixels, *gp_pixels;
    int tv_pitch, gp_pitch;

  // Bloqueamos ambas texturas en la GPU para acceder a sus arreglos de memoria RAM virtuales
    if (SDL_LockTexture(g_texture_tv, &g_sdl_renderer_rect, &tv_pixels, &tv_pitch) == 0) {
      if (SDL_LockTexture(g_texture_gamepad, &g_sdl_renderer_rect, &gp_pixels, &gp_pitch) == 0) {
        
      // Copia masiva de alta velocidad en memoria RAM/VRAM
      // ANOTACIÓN: Copiamos los píxeles de manera compacta y ultra rápida en un solo bloque.
      // g_sdl_renderer_rect.h es la altura (240) y tv_pitch es el ancho en bytes exacto.
      // Esto clona de inmediato CUALQUIER elemento en pantalla de forma idéntica.		
        memcpy(gp_pixels, tv_pixels, g_sdl_renderer_rect.h * tv_pitch);

      // Desbloqueamos la textura del GamePad para enviar los datos nuevos a su GPU        
        SDL_UnlockTexture(g_texture_gamepad);
      }
    // Desbloqueamos la textura de la TV	  
      SDL_UnlockTexture(g_texture_tv);
    }
  } 
  else if (g_dual_screen_mode == kDualScreen_AlwaysMap) {
    // =============================================================================
    // ANOTACIÓN: MAPA EN TIEMPO REAL FIJO EN GAMEPAD
    // =============================================================================
    void *gp_pixels = NULL;
    int gp_pitch = 0;
    
    if (SDL_LockTexture(g_texture_gamepad, &g_sdl_renderer_rect, &gp_pixels, &gp_pitch) == 0) {
      
      // ANOTACIÓN: En lugar de copiar la TV, llamamos a la rutina interna que extrae
      // los datos del mapa de exploración de la RAM y los dibuja aquí.
      // Nota: Si el juego nativo no tiene esta función expuesta, podemos leer directamente
      // el arreglo de la VRAM correspondiente a las capas de fondo (BG3/BG4) del menú.
      Ppu_RenderPauseMapDirect(gp_pixels, gp_pitch);
      
      SDL_UnlockTexture(g_texture_gamepad);
    }
  }

  // 3. ENVIAR SEÑAL DE VIDEO A LA TELEVISIÓN
  SDL_RenderClear(g_renderer_tv);
  SDL_RenderCopy(g_renderer_tv, g_texture_tv, &g_sdl_renderer_rect, NULL);
  SDL_RenderPresent(g_renderer_tv);

  // 4. ENVIAR SEÑAL DE VIDEO AL GAMEPAD
  // RENDERIZADO EN LA PANTALLA SECUNDARIA
  // ANOTACIÓN: Ahora que "g_texture_gamepad" recibió una copia exacta de los datos
  // y pertenece legalmente a "g_renderer_gamepad", la ventana vuelve a iluminarse.  
  SDL_RenderClear(g_renderer_gamepad);
  // Importante: Seguimos usando g_texture_gamepad para respetar las reglas de SDL2
  SDL_RenderCopy(g_renderer_gamepad, g_texture_gamepad, &g_sdl_renderer_rect, NULL); 
  SDL_RenderPresent(g_renderer_gamepad); 
}


static const struct RendererFuncs kSdlRendererFuncs = {
  &SdlRenderer_Init,
  &SdlRenderer_Destroy,
  &SdlRenderer_BeginDraw,
  &SdlRenderer_EndDraw,
};

#undef main
int main(int argc, char** argv) {
#ifdef __SWITCH__
  SwitchImpl_Init();
#endif
  argc--, argv++;
  const char *config_file = NULL;
  if (argc >= 2 && strcmp(argv[0], "--config") == 0) {
    config_file = argv[1];
    argc -= 2, argv += 2;
  } else {
    SwitchDirectory();
  }
  if (argc >= 1 && strcmp(argv[0], "--debug") == 0) {
    g_debug_flag = true;
    argc -= 1, argv += 1;
  }
  ParseConfigFile(config_file);

  g_snes_width = (g_config.extended_aspect_ratio * 2 + 256);
  g_snes_height = 240;
  g_ppu_render_flags = g_config.new_renderer * kPpuRenderFlags_NewRenderer |
    g_config.enhanced_mode7 * kPpuRenderFlags_4x4Mode7 |
    g_config.extend_y * kPpuRenderFlags_Height240 |
    g_config.no_sprite_limits * kPpuRenderFlags_NoSpriteLimits;

  msu_enabled = g_config.enable_msu;

  if (g_config.fullscreen == 1)
    g_win_flags ^= SDL_WINDOW_FULLSCREEN_DESKTOP;
  else if (g_config.fullscreen == 2)
    g_win_flags ^= SDL_WINDOW_FULLSCREEN;

  g_current_window_scale = (g_config.window_scale == 0) ? 2 : IntMin(g_config.window_scale, kMaxWindowScale);

  if (g_config.audio_freq < 11025 || g_config.audio_freq > 48000)
    g_config.audio_freq = kDefaultFreq;

  if (g_config.audio_channels < 1 || g_config.audio_channels > 2)
    g_config.audio_channels = kDefaultChannels;

  if (g_config.audio_samples <= 0 || ((g_config.audio_samples & (g_config.audio_samples - 1)) != 0))
    g_config.audio_samples = kDefaultSamples;

  if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
    printf("Failed to init SDL: %s\n", SDL_GetError());
    return 1;
  }

  bool custom_size = g_config.window_width != 0 && g_config.window_height != 0;
  int window_width = custom_size ? g_config.window_width : g_current_window_scale * g_snes_width;
  int window_height = custom_size ? g_config.window_height : g_current_window_scale * g_snes_height;

  if (g_config.output_method == kOutputMethod_OpenGL) {
    g_win_flags |= SDL_WINDOW_OPENGL;
    OpenGLRenderer_Create(&g_renderer_funcs);
  } else {
    g_renderer_funcs = kSdlRendererFuncs;
  }

  const char* filename = argv[0] ? argv[0] : "sm.smc";
  Snes *snes = SnesInit(filename);

  if(snes == NULL) {
  #ifdef __SWITCH__
    ThrowMissingROM();
  #else
    char buf[256];
    snprintf(buf, sizeof(buf), "unable to load rom: %s", filename);
    Die(buf);
  #endif
    return 1;
  }

  // --- RESTAURADO Y CORREGIDO ---
  // Creamos la ventana de Windows base y llamamos al inicializador de renderizado
  g_window = SDL_CreateWindow(kWindowTitle, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              window_width, window_height, g_win_flags);
  if (g_window == NULL) {
    printf("Failed to create window: %s\n", SDL_GetError());
    return 1;
  }

  if (!g_renderer_funcs.Initialize(g_window)) {
    Die("Failed to initialize renderer");
  }
  // ---------------------------------

  g_audio_mutex = SDL_CreateMutex();
  if (!g_audio_mutex) Die("No mutex");

  g_spc_player = SpcPlayer_Create();
  SpcPlayer_Initialize(g_spc_player);

  bool enable_audio = true;
  if (enable_audio) {
    SDL_AudioSpec want = { 0 }, have;
    want.freq = 44100;
    want.format = AUDIO_S16;
    want.channels = 2;
    want.samples = 2048;
    want.callback = &AudioCallback;
    g_audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (g_audio_device == 0) {
      printf("Failed to open audio device: %s\n", SDL_GetError());
      return 1;
    }
    g_audio_channels = 2;
    g_frames_per_block = (534 * have.freq) / 32000;
    g_audiobuffer = (uint8 *)malloc(g_frames_per_block * have.channels * sizeof(int16));
  }

  PpuBeginDrawing(snes->snes_ppu, g_pixels, 256 * 4, 0);
  PpuBeginDrawing(snes->my_ppu, g_my_pixels, 256 * 4, 0);

#if defined(_WIN32)
  _mkdir("saves");
#else
  mkdir("saves", 0755);
#endif

  RtlReadSram();

  for (int i = 0; i < SDL_NumJoysticks(); i++)
    OpenOneGamepad(i);

  if (g_config.autosave)
    HandleCommand(kKeys_Load + 0, true);

  bool running = true;
  uint32 lastTick = SDL_GetTicks();
  uint32 curTick = 0;
  uint32 frameCtr = 0;
  uint8 audiopaused = true;
  bool has_bug_in_title = false;

  while (running) {
    SDL_Event event;

while (SDL_PollEvent(&event)) {
      switch (event.type) {
      case SDL_QUIT:
        // Cierre global del sistema (ej. Alt+F4 o cerrar desde la barra de tareas)
        running = false;
        break;

      // =============================================================================
      // ANOTACIÓN: SOLUCIÓN DEL JUEGO INMORTAL (EVENTOS DE VENTANA)
      // =============================================================================
      // Al trabajar con múltiples ventanas creadas de forma nativa, necesitamos 
      // interceptar de forma precisa cuál ventana ha recibido la orden de destrucción de la "X".
      // =============================================================================
      case SDL_WINDOWEVENT:
        if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
          printf("Se detectó la solicitud de cierre desde una de las ventanas del juego.\n");
          running = false; // Forzamos la salida del bucle principal de forma segura
        }
        break;

      case SDL_CONTROLLERDEVICEADDED:
        OpenOneGamepad(event.cdevice.which);
        break;
      case SDL_CONTROLLERAXISMOTION:
        HandleGamepadAxisInput(event.caxis.which, event.caxis.axis, event.caxis.value);
        break;
      case SDL_CONTROLLERBUTTONDOWN:
      case SDL_CONTROLLERBUTTONUP: {
        int b = RemapSdlButton(event.cbutton.button);
        if (b >= 0)
          HandleGamepadInput(b, event.type == SDL_CONTROLLERBUTTONDOWN);
        break;
      }
      case SDL_MOUSEWHEEL:
        if (SDL_GetModState() & KMOD_CTRL && event.wheel.y != 0)
          ChangeWindowScale(event.wheel.y > 0 ? 1 : -1);
        break;
      case SDL_MOUSEBUTTONDOWN:
        if (event.button.button == SDL_BUTTON_LEFT && event.button.state == SDL_PRESSED && event.button.clicks == 2) {
          if ((g_win_flags & SDL_WINDOW_FULLSCREEN_DESKTOP) == 0 && (g_win_flags & SDL_WINDOW_FULLSCREEN) == 0 && SDL_GetModState() & KMOD_SHIFT) {
            g_win_flags ^= SDL_WINDOW_BORDERLESS;
            SDL_SetWindowBordered(g_window, (g_win_flags & SDL_WINDOW_BORDERLESS) == 0 ? SDL_TRUE : SDL_FALSE);
          }
        }
        break;
      case SDL_KEYDOWN:
        HandleInput(event.key.keysym.sym, event.key.keysym.mod, true);
        break;
      case SDL_KEYUP:
        HandleInput(event.key.keysym.sym, event.key.keysym.mod, false);
        break;
      }
    }

    if (g_paused != audiopaused) {
      audiopaused = g_paused;
      if (g_audio_device)
        SDL_PauseAudioDevice(g_audio_device, audiopaused);
    }

    if (g_paused) {
      SDL_Delay(16);
      continue;
    }

    int inputs = g_input1_state;
    if (g_input1_state & 0xf0)
      g_gamepad_buttons = 0;
    inputs |= g_gamepad_buttons;

    uint8 is_replay = RtlRunFrame(inputs);

    frameCtr++;
    g_snes->disableRender = (g_turbo ^ (is_replay & g_replay_turbo)) && (frameCtr & (g_turbo ? 0xf : 0x7f)) != 0;

    if (!g_snes->disableRender)
      DrawPpuFrameWithPerf();

    bool want_bug_in_title = (g_got_mismatch_count != 0);
    if (want_bug_in_title != has_bug_in_title) {
      has_bug_in_title = want_bug_in_title;
      char title[60];
      if (want_bug_in_title) {
        snprintf(title, sizeof(title), "%s | BUG FOUND!", kWindowTitle);
        SDL_SetWindowTitle(g_window, title);
      } else {
        SDL_SetWindowTitle(g_window, kWindowTitle);
      }
    }

    curTick = SDL_GetTicks();

    if (!g_snes->disableRender && !g_config.disable_frame_delay) {
      static const uint8 delays[3] = { 17, 17, 16 };
      lastTick += delays[frameCtr % 3];

      if (lastTick > curTick) {
        uint32 delta = lastTick - curTick;
        if (delta > 500) {
          lastTick = curTick - 500;
          delta = 500;
        }
        SDL_Delay(delta);
      } else if (curTick - lastTick > 500) {
        lastTick = curTick;
      }
    }
  }

  if (g_config.autosave)
    HandleCommand(kKeys_Save + 0, true);

  SDL_PauseAudioDevice(g_audio_device, 1);
  SDL_CloseAudioDevice(g_audio_device);
  SDL_DestroyMutex(g_audio_mutex);
  free(g_audiobuffer);

  g_renderer_funcs.Destroy();

#ifdef __SWITCH__
  SwitchImpl_Exit();
#endif

  SDL_DestroyWindow(g_window_tv);
  SDL_Quit();
  return 0;
}

static void RenderDigit(uint8 *dst, size_t pitch, int digit, uint32 color, bool big) {
  static const uint8 kFont[] = {
    0x1c, 0x36, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x36, 0x1c,
    0x18, 0x1c, 0x1e, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7e,
    0x3e, 0x63, 0x60, 0x30, 0x18, 0x0c, 0x06, 0x03, 0x63, 0x7f,
    0x3e, 0x63, 0x60, 0x60, 0x3c, 0x60, 0x60, 0x60, 0x63, 0x3e,
    0x30, 0x38, 0x3c, 0x36, 0x33, 0x7f, 0x30, 0x30, 0x30, 0x78,
    0x7f, 0x03, 0x03, 0x03, 0x3f, 0x60, 0x60, 0x60, 0x63, 0x3e,
    0x1c, 0x06, 0x03, 0x03, 0x3f, 0x63, 0x63, 0x63, 0x63, 0x3e,
    0x7f, 0x63, 0x60, 0x60, 0x30, 0x18, 0x0c, 0x0c, 0x0c, 0x0c,
    0x3e, 0x63, 0x63, 0x63, 0x3e, 0x63, 0x63, 0x63, 0x63, 0x3e,
    0x3e, 0x63, 0x63, 0x63, 0x7e, 0x60, 0x60, 0x60, 0x30, 0x1e,
  };
  const uint8 *p = kFont + digit * 10;
  if (!big) {
    for (int y = 0; y < 10; y++, dst += pitch) {
      int v = *p++;
      for (int x = 0; v; x++, v >>= 1) {
        if (v & 1)
          ((uint32 *)dst)[x] = color;
      }
    }
  } else {
    for (int y = 0; y < 10; y++, dst += pitch * 2) {
      int v = *p++;
      for (int x = 0; v; x++, v >>= 1) {
        if (v & 1) {
          ((uint32 *)dst)[x * 2 + 1] = ((uint32 *)dst)[x * 2] = color;
          ((uint32 *)(dst + pitch))[x * 2 + 1] = ((uint32 *)(dst + pitch))[x * 2] = color;
        }
      }
    }
  }
}

static void RenderNumber(uint8 *dst, size_t pitch, int n, uint8 big) {
  char buf[32], *s;
  int i;
  sprintf(buf, "%d", n);
  for (s = buf, i = 2 * 4; *s; s++, i += 8 * 4)
    RenderDigit(dst + ((pitch + i + 4) << big), pitch, *s - '0', 0x404040, big);
  for (s = buf, i = 2 * 4; *s; s++, i += 8 * 4)
    RenderDigit(dst + (i << big), pitch, *s - '0', 0xffffff, big);
}

static void HandleCommand(uint32 j, bool pressed) {
  if (j <= kKeys_Controls_Last) {
    static const uint8 kKbdRemap[] = { 0, 4, 5, 6, 7, 2, 3, 8, 0, 9, 1, 10, 11 };
    if (pressed)
      g_input1_state |= 1 << kKbdRemap[j];
    else
      g_input1_state &= ~(1 << kKbdRemap[j]);
    return;
  }

  if (j == kKeys_Turbo) {
    g_turbo = pressed;
    return;
  }

  if (!pressed)
    return;
  if (j <= kKeys_Load_Last) {
    RtlSaveLoad(kSaveLoad_Load, j - kKeys_Load);
  } else if (j <= kKeys_Save_Last) {
    RtlSaveLoad(kSaveLoad_Save, j - kKeys_Save);
  } else if (j <= kKeys_Replay_Last) {
    RtlSaveLoad(kSaveLoad_Replay, j - kKeys_Replay);
  } else if (j <= kKeys_LoadRef_Last) {
    RtlSaveLoad(kSaveLoad_Load, 256 + j - kKeys_LoadRef);
  } else if (j <= kKeys_ReplayRef_Last) {
    RtlSaveLoad(kSaveLoad_Replay, 256 + j - kKeys_ReplayRef);
  } else {
    switch (j) {
    case kKeys_CheatLife: RtlCheat('w'); break;
    case kKeys_CheatJump: RtlCheat('q'); break;
    case kKeys_ToggleWhichFrame:
      g_other_image = !g_other_image;
      break;
    case kKeys_ClearKeyLog: RtlClearKeyLog(); break;
    case kKeys_StopReplay: RtlStopReplay(); break;
    case kKeys_Fullscreen:
      g_win_flags ^= SDL_WINDOW_FULLSCREEN_DESKTOP;
      SDL_SetWindowFullscreen(g_window, g_win_flags & SDL_WINDOW_FULLSCREEN_DESKTOP);
      g_cursor = !g_cursor;
      SDL_ShowCursor(g_cursor);
      break;
    case kKeys_Reset:
      RtlReset(1);
      break;
    case kKeys_Pause: g_paused = !g_paused; break;
    case kKeys_PauseDimmed:
      g_paused = !g_paused;
#ifdef _WIN32
      if (g_paused) {
        SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 159);
        SDL_RenderFillRect(g_renderer, NULL);
        SDL_RenderPresent(g_renderer);
      }
#endif
      break;
    case kKeys_ReplayTurbo: g_replay_turbo = !g_replay_turbo; break;
    case kKeys_WindowBigger: ChangeWindowScale(1); break;
    case kKeys_WindowSmaller: ChangeWindowScale(-1); break;
    case kKeys_DisplayPerf: g_display_perf ^= 1; break;
    case kKeys_ToggleRenderer:
      g_ppu_render_flags ^= kPpuRenderFlags_NewRenderer;
      g_new_ppu = (g_ppu_render_flags & kPpuRenderFlags_NewRenderer) != 0;
      break;
    case kKeys_VolumeUp:
    case kKeys_VolumeDown: HandleVolumeAdjustment(j == kKeys_VolumeUp ? 1 : -1); break;
    default: assert(0);
    }
  }
}

static void HandleInput(int keyCode, int keyMod, bool pressed) {
  int j = FindCmdForSdlKey(keyCode, (SDL_Keymod)keyMod);
  if (j != 0)
    HandleCommand(j, pressed);
}

static void OpenOneGamepad(int i) {
  if (SDL_IsGameController(i)) {
    SDL_GameController *controller = SDL_GameControllerOpen(i);
    if (!controller)
      fprintf(stderr, "Could not open gamepad %d: %s\n", i, SDL_GetError());
  }
}

static int RemapSdlButton(int button) {
  switch (button) {
  case SDL_CONTROLLER_BUTTON_A: return kGamepadBtn_A;
  case SDL_CONTROLLER_BUTTON_B: return kGamepadBtn_B;
  case SDL_CONTROLLER_BUTTON_X: return kGamepadBtn_X;
  case SDL_CONTROLLER_BUTTON_Y: return kGamepadBtn_Y;
  case SDL_CONTROLLER_BUTTON_BACK: return kGamepadBtn_Back;
  case SDL_CONTROLLER_BUTTON_GUIDE: return kGamepadBtn_Guide;
  case SDL_CONTROLLER_BUTTON_START: return kGamepadBtn_Start;
  case SDL_CONTROLLER_BUTTON_LEFTSTICK: return kGamepadBtn_L3;
  case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return kGamepadBtn_R3;
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return kGamepadBtn_L1;
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return kGamepadBtn_R1;
  case SDL_CONTROLLER_BUTTON_DPAD_UP: return kGamepadBtn_DpadUp;
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return kGamepadBtn_DpadDown;
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return kGamepadBtn_DpadLeft;
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return kGamepadBtn_DpadRight;
  default: return -1;
  }
}

static void HandleGamepadInput(int button, bool pressed) {
  if (!!(g_gamepad_modifiers & (1 << button)) == pressed)
    return;
  g_gamepad_modifiers ^= 1 << button;
  if (pressed)
    g_gamepad_last_cmd[button] = FindCmdForGamepadButton(button, g_gamepad_modifiers);
  if (g_gamepad_last_cmd[button] != 0)
    HandleCommand(g_gamepad_last_cmd[button], pressed);
}

static void HandleVolumeAdjustment(int volume_adjustment) {
#if SYSTEM_VOLUME_MIXER_AVAILABLE
  int current_volume = GetApplicationVolume();
  int new_volume = IntMin(IntMax(0, current_volume + volume_adjustment * 5), 100);
  SetApplicationVolume(new_volume);
  printf("[System Volume]=%i\n", new_volume);
#else
  g_sdl_audio_mixer_volume = IntMin(IntMax(0, g_sdl_audio_mixer_volume + volume_adjustment * (SDL_MIX_MAXVOLUME >> 4)), SDL_MIX_MAXVOLUME);
  printf("[SDL mixer volume]=%i\n", g_sdl_audio_mixer_volume);
#endif
}

static float ApproximateAtan2(float y, float x) {
  uint32 sign_mask = 0x80000000;
  float b = 0.596227f;
  uint32 ux_s = sign_mask & *(uint32 *)&x;
  uint32 uy_s = sign_mask & *(uint32 *)&y;
  float q = (float)((~ux_s & uy_s) >> 29 | ux_s >> 30);
  float bxy_a = b * x * y;
  if (bxy_a < 0.0f) bxy_a = -bxy_a;
  float num = bxy_a + y * y;
  float atan_1q = num / (x * x + bxy_a + num + 0.000001f);
  uint32_t uatan_2q = (ux_s ^ uy_s) | *(uint32 *)&atan_1q;
  return q + *(float *)&uatan_2q;
}

static void HandleGamepadAxisInput(int gamepad_id, int axis, int value) {
  static int last_gamepad_id, last_x, last_y;
  if (axis == SDL_CONTROLLER_AXIS_LEFTX || axis == SDL_CONTROLLER_AXIS_LEFTY) {
    if (last_gamepad_id != gamepad_id) {
      if (value > -16000 && value < 16000)
        return;
      last_gamepad_id = gamepad_id;
      last_x = last_y = 0;
    }
    *(axis == SDL_CONTROLLER_AXIS_LEFTX ? &last_x : &last_y) = value;
    int buttons = 0;
    if (last_x * last_x + last_y * last_y >= 10000 * 10000) {
      static const uint8 kSegmentToButtons[8] = {
        1 << 4,
        1 << 4 | 1 << 7,
        1 << 7,
        1 << 7 | 1 << 5,
        1 << 5,
        1 << 5 | 1 << 6,
        1 << 6,
        1 << 6 | 1 << 4,
      };
      uint8 angle = (uint8)(int)(ApproximateAtan2(last_y, last_x) * 64.0f + 0.5f);
      buttons = kSegmentToButtons[(uint8)(angle + 16 + 64) >> 5];
    }
    g_gamepad_buttons = buttons;
  } else if ((axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) {
    if (value < 12000 || value >= 16000)
      HandleGamepadInput(axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? kGamepadBtn_L2 : kGamepadBtn_R2, value >= 12000);
  }
}

static void SwitchDirectory(void) {
  char buf[4096];
  if (!getcwd(buf, sizeof(buf) - 32))
    return;
  size_t pos = strlen(buf);

  for (int step = 0; pos != 0 && step < 3; step++) {
    memcpy(buf + pos, "/sm.ini", 8);
    FILE *f = fopen(buf, "rb");
    if (f) {
      fclose(f);
      buf[pos] = 0;
      if (step != 0) {
        printf("Found sm.ini in %s\n", buf);
        int err = chdir(buf);
        (void)err;
      }
      return;
    }
    pos--;
    while (pos != 0 && buf[pos] != '/' && buf[pos] != '\\')
      pos--;
  }
}