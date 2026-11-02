// src/main.cpp - InputAssist para Geode (Android)
// Asistencia y accesibilidad: alinea clicks al tick de fisica, corrige
// pulsaciones tempranas, estabiliza avion y wave. Todo sin heap en el tick.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/EffectGameObject.hpp>
#include <Geode/ui/Popup.hpp>
#include <cocos2d.h>

using namespace geode::prelude;
namespace cocos = cocos2d;

// ===========================================================================
// PARTE 1/5: CONSTANTES, ESTRUCTURAS Y ESTADO GLOBAL
// ===========================================================================

namespace {

    // -----------------------------------------------------------------------
    // Constantes del sistema
    // -----------------------------------------------------------------------
    constexpr size_t  kCapBuffer     = 64;    // capacidad del ring buffer
    constexpr size_t  kCapErrores    = 60;    // ventana adaptativa
    constexpr int     kTicksMaxSim   = 20;    // horizonte de prediccion
    constexpr float   kRadioEscaneo  = 5.f;   // bloques de escaneo
    constexpr uint8_t kFlagNone      = 0;

    // -----------------------------------------------------------------------
    // Entrada capturada (POD, sin heap)
    // -----------------------------------------------------------------------
    struct Entrada {
        bool     down;      // true = presion, false = liberacion
        int      button;    // 1 = jump, 2 = left, 3 = right
        bool     isP1;      // jugador 1 o 2
        uint32_t tick;      // tick de fisica en el que se capturo
    };

    // -----------------------------------------------------------------------
    // Clasificacion de obstaculos (bitmask para no usar vectores)
    // -----------------------------------------------------------------------
    enum TipoPeligro : uint8_t {
        PELIGRO_NINGUNO   = 0,
        PELIGRO_SOLIDO    = 1 << 0,
        PELIGRO_PINCHO    = 1 << 1,
        PELIGRO_SIERRA    = 1 << 2,
        PELIGRO_PORTAL    = 1 << 3,
        PELIGRO_ORBE      = 1 << 4,
        PELIGRO_PAD       = 1 << 5,
        PELIGRO_HIELO     = 1 << 6,
    };

    struct ObstaculoCercano {
        TipoPeligro tipo;
        float x, y, w, h;    // bounds en coordenadas de mundo
        float distancia;     // distancia horizontal al jugador
    };

    // -----------------------------------------------------------------------
    // Metricas adaptativas (sin heap)
    // -----------------------------------------------------------------------
    struct Metricas {
        float erroresMs[kCapErrores];
        uint32_t n;
        uint32_t idx;
        float    desfaseAdaptativoMs;
        uint32_t corregidos;
        uint32_t totales;
    };

    // -----------------------------------------------------------------------
    // Estado global del mod (todo fijo, sin STL)
    // -----------------------------------------------------------------------
    Entrada         g_buf[kCapBuffer]{};
    size_t          g_cabeza = 0;
    size_t          g_cola   = 0;

    Metricas        g_metricas{};
    ObstaculoCercano g_peligrosCercanos[32]{};
    size_t          g_numPeligros = 0;

    PlayLayer*      g_playLayer = nullptr;
    PlayerObject*   g_player1   = nullptr;
    PlayerObject*   g_player2   = nullptr;

    uint32_t        g_tickFisico      = 0;
    uint32_t        g_toleranciaTicks = 7;
    float           g_targetHz        = 60.f;
    float           g_altitudObjetivo = 0.f;
    float           g_waveAngulo      = 0.f;

    bool            g_activo          = false;
    bool            g_reemitir        = false;
    bool            g_altitudFijada   = false;

    // -----------------------------------------------------------------------
    // Helpers basicos
    // -----------------------------------------------------------------------
    inline bool bufferLleno() { return ((g_cabeza + 1) % kCapBuffer) == g_cola; }
    inline bool bufferVacio() { return g_cabeza == g_cola; }

    inline void limpiarBuffer() {
        g_cabeza = 0;
        g_cola   = 0;
    }

    inline void pushEntrada(bool down, int button, bool isP1) {
        if (bufferLleno()) g_cola = (g_cola + 1) % kCapBuffer;
        g_buf[g_cabeza] = Entrada{ down, button, isP1, g_tickFisico };
        g_cabeza = (g_cabeza + 1) % kCapBuffer;
    }

    inline const Entrada* frenteBuffer() {
        return bufferVacio() ? nullptr : &g_buf[g_cola];
    }

    inline void popFrenteBuffer() {
        if (!bufferVacio()) g_cola = (g_cola + 1) % kCapBuffer;
    }

    inline void resetMetricas() {
        g_metricas = Metricas{};
        g_metricas.desfaseAdaptativoMs = 0.f;
    }

    // conversion ms -> ticks con la tasa detectada
    inline uint32_t msATicks(float ms) {
        float hz = g_targetHz > 1.f ? g_targetHz : 60.f;
        float t  = (ms * hz) / 1000.f;
        uint32_t r = static_cast<uint32_t>(t + 0.5f);
        return r == 0 ? 1 : r;
    }

    // -----------------------------------------------------------------------
    // Deteccion de la tasa de refresco
    // -----------------------------------------------------------------------
    void detectarTasaRefresco() {
        g_targetHz = 60.f;
        if (auto* app = CCApplication::get()) {
            float r = app->getTargetTickRate();
            if (r > 1.f && r < 240.f) g_targetHz = r;
        }
    }

    // -----------------------------------------------------------------------
    // Recalcular tolerancia desde los settings
    // -----------------------------------------------------------------------
    void recalcularTolerancia() {
        int ms = static_cast<int>(Mod::get()->getSettingValue<int64_t>("tolerancia-ms"));
        g_toleranciaTicks = msATicks(static_cast<float>(ms));
    }

    // -----------------------------------------------------------------------
    // Emitir una entrada directamente al jugador (bypass de handleButton)
    // -----------------------------------------------------------------------
    void emitirEntrada(const Entrada& e) {
        PlayerObject* target = e.isP1 ? g_player1 : g_player2;
        if (!target) return;
        if (e.down) target->pushButton(static_cast<PlayerButton>(e.button));
        else        target->releaseButton(static_cast<PlayerButton>(e.button));
    }
}// ===========================================================================
// PARTE 2/5: ESCANER DE OBSTACULOS Y SOLVER DE TRAYECTORIA
// ===========================================================================

namespace {

    // -----------------------------------------------------------------------
    // Clasifica un GameObject segun su tipo. Devuelve una bitmask.
    // -----------------------------------------------------------------------
    inline TipoPeligro clasificarObjeto(GameObject* obj) {
        if (!obj) return PELIGRO_NINGUNO;
        auto t = obj->m_objectType;
        if (t == GameObjectType::Solid)  return PELIGRO_SOLIDO;
        if (t == GameObjectType::Hazard) {
            int id = obj->m_objectID;
            // IDs conocidos de sierras grandes
            if (id == 103 || id == 104 || id == 105) return PELIGRO_SIERRA;
            return PELIGRO_PINCHO;
        }
        if (t == GameObjectType::Portal) return PELIGRO_PORTAL;
        if (t == GameObjectType::Special) {
            int id = obj->m_objectID;
            // IDs comunes de orbes y pads
            if (id >= 17 && id <= 24) return PELIGRO_ORBE;
            if (id >= 35 && id <= 41) return PELIGRO_PAD;
        }
        return PELIGRO_NINGUNO;
    }

    // -----------------------------------------------------------------------
    // Escanea m_objects en un radio alrededor del jugador. Llena g_peligrosCercanos.
    // Devuelve el numero de obstaculos encontrados.
    // -----------------------------------------------------------------------
    size_t escanearObstaculos() {
        g_numPeligros = 0;
        if (!g_playLayer || !g_player1) return 0;

        auto* arrayObj = g_playLayer->m_objects;
        if (!arrayObj) return 0;

        float px = g_player1->getPositionX();
        float py = g_player1->getPositionY();
        // radio en pixeles (1 bloque ~ 30 px)
        float radioPx = kRadioEscaneo * 30.f;
        float radioSq = radioPx * radioPx;

        for (auto* nodo : CCArrayExt<CCNode*>(arrayObj)) {
            auto* obj = typeinfo_cast<GameObject*>(nodo);
            if (!obj) continue;

            float ox = obj->getPositionX();
            float oy = obj->getPositionY();
            float dx = ox - px;
            float dy = oy - py;
            if (dx < -radioPx || dx > radioPx) continue;  // descarte rapido
            if (dy < -radioPx || dy > radioPx) continue;
            if (dx * dx + dy * dy > radioSq) continue;

            TipoPeligro tipo = clasificarObjeto(obj);
            if (tipo == PELIGRO_NINGUNO) continue;

            if (g_numPeligros >= 32) break;

            auto rect = obj->getObjectRect();
            ObstaculoCercano& o = g_peligrosCercanos[g_numPeligros++];
            o.tipo      = tipo;
            o.x         = rect.getMinX();
            o.y         = rect.getMinY();
            o.w         = rect.getWidth();
            o.h         = rect.getHeight();
            o.distancia = dx < 0 ? -dx : dx;
        }

        return g_numPeligros;
    }

    // -----------------------------------------------------------------------
    // Devuelve true si hay un solido o pincho en un rango horizontal cercano.
    // Se usa para decidir si el click es "seguro" en el tick actual.
    // -----------------------------------------------------------------------
    bool hayPeligroInminente(float radioPx) {
        if (!g_player1) return false;
        for (size_t i = 0; i < g_numPeligros; ++i) {
            const auto& o = g_peligrosCercanos[i];
            if (o.distancia > radioPx) continue;
            if (o.tipo & (PELIGRO_SOLIDO | PELIGRO_PINCHO | PELIGRO_SIERRA)) {
                return true;
            }
        }
        return false;
    }

    // -----------------------------------------------------------------------
    // SOLVER: simula la trayectoria del jugador N ticks hacia delante.
    // Devuelve el primer tick en el que colisiona con un peligro, o -1 si no.
    // -----------------------------------------------------------------------
    struct EstadoSim {
        float x, y, vy;
        bool  grounded;
    };

    int predecirPrimerImpacto(int ticksMax) {
        if (!g_player1) return -1;

        EstadoSim s;
        s.x = g_player1->getPositionX();
        s.y = g_player1->getPositionY();
        s.vy = g_player1->m_yVelocity;
        s.grounded = false;

        // Parametros simplificados por vehiculo
        float gravedad = 0.958199f;
        float vx = 8.4f;              // velocidad horizontal aprox
        bool  esShip = g_player1->m_isShip;
        bool  esUFO  = g_player1->m_isUFO;
        bool  esWave = g_player1->m_isWave;

        for (int t = 1; t <= ticksMax; ++t) {
            // Avanza posicion
            s.x += vx;
            if (esWave) {
                s.vy = (g_waveAngulo) * vx;
            } else if (esShip) {
                s.vy += (s.grounded ? -gravedad : gravedad * 0.5f);
            } else if (esUFO) {
                s.vy += gravedad;
            } else {
                s.vy -= gravedad;
            }
            s.y += s.vy;

            // Comprueba colision con cada peligro cercano
            for (size_t i = 0; i < g_numPeligros; ++i) {
                const auto& o = g_peligrosCercanos[i];
                if (!(o.tipo & (PELIGRO_SOLIDO | PELIGRO_PINCHO | PELIGRO_SIERRA))) continue;
                if (s.x + 15.f >= o.x && s.x - 15.f <= o.x + o.w &&
                    s.y + 15.f >= o.y && s.y - 15.f <= o.y + o.h) {
                    return t;
                }
            }
        }
        return -1;
    }
}// ===========================================================================
// PARTE 3/5: CORRECCION, WAVE, VUELO Y MODO ADAPTATIVO
// ===========================================================================

namespace {

    // -----------------------------------------------------------------------
    // Registra el error de un click respecto al tick ideal (media movil).
    // -----------------------------------------------------------------------
    void registrarError(float errorMs) {
        g_metricas.erroresMs[g_metricas.idx] = errorMs;
        g_metricas.idx = (g_metricas.idx + 1) % kCapErrores;
        if (g_metricas.n < kCapErrores) ++g_metricas.n;
    }

    // -----------------------------------------------------------------------
    // Calcula la media del error en la ventana adaptativa.
    // -----------------------------------------------------------------------
    float mediaErrorMs() {
        if (g_metricas.n == 0) return 0.f;
        float suma = 0.f;
        for (uint32_t i = 0; i < g_metricas.n; ++i) suma += g_metricas.erroresMs[i];
        return suma / static_cast<float>(g_metricas.n);
    }

    // -----------------------------------------------------------------------
    // Actualiza el desfase adaptativo con convergencia suave (factor 0.15).
    // -----------------------------------------------------------------------
    void actualizarDesfaseAdaptativo() {
        float m = mediaErrorMs();
        g_metricas.desfaseAdaptativoMs += -m * 0.15f;
        // Clamp a +-33 ms (~2 ticks a 60 Hz)
        if (g_metricas.desfaseAdaptativoMs >  33.f) g_metricas.desfaseAdaptativoMs =  33.f;
        if (g_metricas.desfaseAdaptativoMs < -33.f) g_metricas.desfaseAdaptativoMs = -33.f;
    }

    // -----------------------------------------------------------------------
    // Emite las entradas pendientes que ya toca emitir.
    // -----------------------------------------------------------------------
    void procesarBuffer() {
        while (!bufferVacio()) {
            const Entrada* e = frenteBuffer();
            if (!e) break;

            // Desfase manual + adaptativo (en ticks)
            int desfaseFrames = static_cast<int>(Mod::get()->getSettingValue<int64_t>("desfase-frames"));
            if (Mod::get()->getSettingValue<bool>("modo-adaptativo")) {
                desfaseFrames += static_cast<int>(g_metricas.desfaseAdaptativoMs / (1000.f / g_targetHz));
            }

            int64_t tickObjetivo = static_cast<int64_t>(e->tick) + desfaseFrames;
            if (tickObjetivo < 0) tickObjetivo = 0;

            if (static_cast<uint32_t>(tickObjetivo) > g_tickFisico) break;    // aun futuro

            // Si estamos fuera de tolerancia por detras, descartamos
            if (g_tickFisico - static_cast<uint32_t>(tickObjetivo) > g_toleranciaTicks) {
                popFrenteBuffer();
                continue;
            }

            // Correccion temprana: si el click es jump y hay peligro inminente,
            // retardamos hasta el siguiente tick (evita saltar contra un pincho).
            bool esJump = (e->button == 1);
            bool peligro = hayPeligroInminente(60.f);
            if (esJump && e->down && peligro && (g_tickFisico - static_cast<uint32_t>(tickObjetivo)) < 2) {
                // esperar un tick mas
                break;
            }

            emitirEntrada(*e);
            ++g_metricas.totales;
            if (tickObjetivo != static_cast<int64_t>(e->tick)) {
                ++g_metricas.corregidos;
            }
            popFrenteBuffer();
        }
    }

    // -----------------------------------------------------------------------
    // Wave recta: mantiene angulo 0 con micro-pulsos.
    // -----------------------------------------------------------------------
    void aplicarWaveRecta() {
        if (!g_player1 || !g_player1->m_isWave) return;
        if (!Mod::get()->getSettingValue<bool>("wave-recta")) return;
        if (!g_activo) return;
        if (!bufferVacio()) return;  // no interferir si hay entrada del usuario

        // Tolerancia de angulo
        constexpr float umbral = 0.05f;
        if (g_waveAngulo > umbral) {
            // corrige hacia abajo: pulsar brevemente
            g_player1->pushButton(PlayerButton::Jump);
        } else if (g_waveAngulo < -umbral) {
            g_player1->releaseButton(PlayerButton::Jump);
        }
    }

    // -----------------------------------------------------------------------
    // Vuelo recto para ship/UFO: mantiene altitud objetivo con amortiguacion.
    // -----------------------------------------------------------------------
    void aplicarVueloRecto() {
        if (!g_player1) return;
        bool esShip = g_player1->m_isShip;
        bool esUFO  = g_player1->m_isUFO;
        if (!esShip && !esUFO) return;
        if (!Mod::get()->getSettingValue<bool>("vuelo-recto")) return;
        if (!g_activo) return;
        if (!bufferVacio()) return;

        float yActual = g_player1->getPositionY();
        if (!g_altitudFijada) {
            g_altitudObjetivo = yActual;
            g_altitudFijada = true;
            return;
        }

        float error = g_altitudObjetivo - yActual;
        float amort = static_cast<float>(Mod::get()->getSettingValue<double>("amortiguacion-vuelo"));
        float umbralSubida = 5.f * (1.f - amort) + 2.f;
        float umbralBajada = -umbralSubida;

        if (error > umbralSubida) {
            g_player1->pushButton(PlayerButton::Jump);
        } else if (error < umbralBajada) {
            g_player1->releaseButton(PlayerButton::Jump);
        }
    }
}// ===========================================================================
// PARTE 4/5: HOOKS
// ===========================================================================

// ---------------------------------------------------------------------------
// PlayerObject: solo mantenemos referencias y passthrough
// ---------------------------------------------------------------------------
class $modify(InputAssistPlayer, PlayerObject) {
    void pushButton(PlayerButton b) {
        PlayerObject::pushButton(b);
    }
    void releaseButton(PlayerButton b) {
        PlayerObject::releaseButton(b);
    }
};

// ---------------------------------------------------------------------------
// GJBaseGameLayer: captura la intencion del usuario y evita doble emision
// ---------------------------------------------------------------------------
class $modify(InputAssistBase, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        if (!g_activo || g_reemitir) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }
        pushEntrada(down, button, isPlayer1);
    }
};

// ---------------------------------------------------------------------------
// PlayLayer: reloj de fisica, ciclo de vida y orquestacion
// ---------------------------------------------------------------------------
class $modify(InputAssistPlay, PlayLayer) {

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        g_playLayer = this;
        g_player1   = m_player1;
        g_player2   = m_player2;

        std::string modo = Mod::get()->getSettingValue<std::string>("modo-asistencia");
        g_activo = (modo != "desactivado");

        if (g_activo) {
            detectarTasaRefresco();
            recalcularTolerancia();
            limpiarBuffer();
            resetMetricas();
            g_tickFisico      = 0;
            g_altitudFijada   = false;
            g_waveAngulo      = 0.f;
            log::info("InputAssist activo ({} Hz) en modo '{}'", g_targetHz, modo);
        }
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        limpiarBuffer();
        g_tickFisico    = 0;
        g_altitudFijada = false;
        g_waveAngulo    = 0.f;
        recalcularTolerancia();
    }

    void pauseGame(bool p0) {
        PlayLayer::pauseGame(p0);
        limpiarBuffer();
    }

    void onQuit() {
        if (g_playLayer == this) {
            g_playLayer = nullptr;
            g_player1   = nullptr;
            g_player2   = nullptr;
            limpiarBuffer();
        }
        PlayLayer::onQuit();
    }

    void update(float dt) {
        if (g_activo) {
            ++g_tickFisico;

            // Re-sincroniza punteros por si cambian al morir
            g_player1 = m_player1;
            g_player2 = m_player2;

            if (g_player1) {
                // Actualiza angulo de wave segun velocidad
                if (g_player1->m_isWave) {
                    g_waveAngulo = g_player1->m_yVelocity / 8.4f;
                }
            }

            // 1) Escanear obstaculos
            escanearObstaculos();

            // 2) Emitir entradas pendientes
            procesarBuffer();

            // 3) Asistencias activas
            aplicarWaveRecta();
            aplicarVueloRecto();

            // 4) Actualizar desfase adaptativo periodicamente
            if (Mod::get()->getSettingValue<bool>("modo-adaptativo")
                && (g_tickFisico % 30) == 0) {
                actualizarDesfaseAdaptativo();
            }
        }

        PlayLayer::update(dt);
    }
};// ===========================================================================
// PARTE 5/5: HUD, PANEL Y CICLO DE VIDA
// ===========================================================================

// ---------------------------------------------------------------------------
// HUD flotante con estado y metricas
// ---------------------------------------------------------------------------
class AssistHUD : public cocos2d::CCNode {
public:
    static AssistHUD* create() {
        auto* n = new AssistHUD();
        if (n->init()) { n->autorelease(); return n; }
        delete n;
        return nullptr;
    }

    bool init() override {
        if (!CCNode::init()) return false;

        _label = CCLabelBMFont::create("InputAssist ON", "bigFont.fnt");
        _label->setScale(0.5f);
        _label->setAnchorPoint({0.f, 1.f});
        addChild(_label);

        // Fondo semitransparente
        auto* bg = cocos2d::CCLayerColor::create(cocos2d::ccc4(0, 0, 0, 120), 200, 60);
        bg->setAnchorPoint({0.f, 1.f});
        bg->setPosition({-5.f, 10.f});
        addChild(bg, -1);

        setPosition({20.f, cocos2d::CCDirector::get()->getWinSize().height - 80.f});
        scheduleUpdate();
        return true;
    }

    void update(float) override {
        if (!_label) return;
        if (!g_activo) {
            _label->setString("InputAssist OFF");
            return;
        }
        auto media = mediaErrorMs();
        char buf[128];
        std::snprintf(buf, sizeof(buf),
            "IA %s | err %.1fms | corr %u/%u",
            "ON", media, g_metricas.corregidos, g_metricas.totales);
        _label->setString(buf);
    }

    CCLabelBMFont* _label = nullptr;
};

// ---------------------------------------------------------------------------
// Panel de ajustes tactil
// ---------------------------------------------------------------------------
class AssistPanel : public geode::Popup<float> {
public:
    static AssistPanel* create() {
        auto* ret = new AssistPanel();
        if (ret->init(360.f, 300.f, "GJ_square01.png")) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    bool init(float w, float h, const char* title) {
        if (!Popup::init(w, h, title)) return false;
        setTitle("InputAssist");

        // Descripcion
        auto* desc = CCLabelBMFont::create(
            "Modo actual: toca un preset para cambiar.",
            "chatFont.fnt");
        desc->setScale(0.7f);
        desc->setPosition({w * 0.5f, h - 55.f});
        m_mainLayer->addChild(desc);

        // Botones de preset (uno debajo de otro)
        const char* nombres[] = {"Desactivado", "Suave", "Equilibrado", "Agresivo"};
        const char* valores[] = {"desactivado", "suave", "equilibrado", "agresivo"};

        auto* menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        m_mainLayer->addChild(menu);

        for (int i = 0; i < 4; ++i) {
            auto* label = CCLabelBMFont::create(nombres[i], "bigFont.fnt");
            label->setScale(0.6f);
            auto* btn = CCMenuItemSpriteExtra::create(
                label, this,
                menu_selector(AssistPanel::onPreset));
            btn->setTag(i);
            btn->setPosition({w * 0.5f, h - 100.f - i * 38.f});
            menu->addChild(btn);
        }

        // Boton de reset
        auto* resetLabel = CCLabelBMFont::create("Reset metricas", "bigFont.fnt");
        resetLabel->setScale(0.55f);
        auto* resetBtn = CCMenuItemSpriteExtra::create(
            resetLabel, this,
            menu_selector(AssistPanel::onReset));
        resetBtn->setPosition({w * 0.5f, 40.f});
        menu->addChild(resetBtn);

        return true;
    }

    void onPreset(CCObject* sender) {
        auto* item = typeinfo_cast<CCMenuItem*>(sender);
        if (!item) return;
        int idx = item->getTag();
        const char* valores[] = {"desactivado", "suave", "equilibrado", "agresivo"};
        Mod::get()->setSettingValue<std::string>("modo-asistencia", valores[idx]);
        if (idx == 0) {
            g_activo = false;
            limpiarBuffer();
        } else {
            g_activo = true;
        }
    }

    void onReset(CCObject*) {
        resetMetricas();
        FLAlertLayer::create(
            "InputAssist",
            "Metricas reiniciadas.",
            "OK")->show();
    }
};

// ---------------------------------------------------------------------------
// Boton en el menu de pausa para abrir el panel
// ---------------------------------------------------------------------------
class $modify(PauseLayerHook, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        if (!Mod::get()->getSettingValue<bool>("mostrar-hud")) return;

        auto* menu = getChildByID("right-button-menu");
        if (!menu) return;

        auto* spr = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
        if (!spr) return;
        auto* btn = CCMenuItemSpriteExtra::create(
            spr, this,
            menu_selector(PauseLayerHook::abrirPanel));
        menu->addChild(btn);
    }

    void abrirPanel(CCObject*) {
        AssistPanel::create()->show();
    }
};

// ---------------------------------------------------------------------------
// Ciclo de vida: ajustes en caliente
// ---------------------------------------------------------------------------
$on_mod(Loaded) {
    log::info("InputAssist cargado.");

    listenForSettingChanges<int64_t>("tolerancia-ms", [](int64_t) {
        recalcularTolerancia();
    });

    listenForSettingChanges<std::string>("modo-asistencia", [](std::string modo) {
        g_activo = (modo != "desactivado");
        if (!g_activo) limpiarBuffer();
    });

    listenForSettingChanges<bool>("mostrar-hud", [](bool mostrar) {
        // El HUD se recrea la proxima vez que entres a un nivel
        log::info("HUD {}", mostrar ? "visible" : "oculto");
    });
}
