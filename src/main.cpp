#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;

namespace {
    constexpr size_t kCapBuffer = 64;
    struct Entrada { bool down; int button; bool isP1; uint32_t tick; };
    Entrada g_buf[kCapBuffer]{};
    size_t g_cabeza = 0, g_cola = 0;

    struct Metricas { float erroresMs[60]; uint32_t n, idx; float desfase; uint32_t corregidos, totales; };
    Metricas g_metricas{};

    uint32_t g_tickFisico = 0, g_toleranciaTicks = 7;
    float g_targetHz = 60.f, g_altitudObjetivo = 0.f, g_waveAngulo = 0.f;
    bool g_activo = false, g_altitudFijada = false;
    bool g_emitiendo = false;

    inline bool bufferLleno() { return ((g_cabeza + 1) % kCapBuffer) == g_cola; }
    inline bool bufferVacio() { return g_cabeza == g_cola; }
    inline void limpiarBuffer() { g_cabeza = 0; g_cola = 0; }

    inline void pushEntrada(bool down, int button, bool isP1) {
        if (bufferLleno()) g_cola = (g_cola + 1) % kCapBuffer;
        g_buf[g_cabeza] = Entrada{ down, button, isP1, g_tickFisico };
        g_cabeza = (g_cabeza + 1) % kCapBuffer;
    }

    inline void resetMetricas() { g_metricas = Metricas{}; }

    inline uint32_t msATicks(float ms) {
        float hz = g_targetHz > 1.f ? g_targetHz : 60.f;
        uint32_t r = static_cast<uint32_t>((ms * hz) / 1000.f + 0.5f);
        return r == 0 ? 1 : r;
    }

    void recalcularTolerancia() {
        int ms = static_cast<int>(Mod::get()->getSettingValue<int64_t>("tolerancia-ms"));
        g_toleranciaTicks = msATicks(static_cast<float>(ms));
    }

    void procesarBuffer() {
        while (!bufferVacio()) {
            const Entrada* e = &g_buf[g_cola];
            int desfaseFrames = static_cast<int>(Mod::get()->getSettingValue<int64_t>("desfase-frames"));
            if (Mod::get()->getSettingValue<bool>("modo-adaptativo")) {
                desfaseFrames += static_cast<int>(g_metricas.desfase / (1000.f / g_targetHz));
            }
            int64_t tickObjetivo = static_cast<int64_t>(e->tick) + desfaseFrames;
            if (tickObjetivo < 0) tickObjetivo = 0;
            if (static_cast<uint32_t>(tickObjetivo) > g_tickFisico) break;

            if (g_tickFisico - static_cast<uint32_t>(tickObjetivo) > g_toleranciaTicks) {
                g_cola = (g_cola + 1) % kCapBuffer; continue;
            }

            // Log para debug (lo verás en la consola de Geode si lo activas)
            log::debug("Emitiendo click: down={} button={}", e->down, e->button);

            g_emitiendo = true;
            if (PlayLayer::get()) {
                // Llamamos a la funcion original del juego de forma segura
                static_cast<GJBaseGameLayer*>(PlayLayer::get())->handleButton(e->down, e->button, e->isP1);
            }
            g_emitiendo = false;

            ++g_metricas.totales;
            if (tickObjetivo != static_cast<int64_t>(e->tick)) ++g_metricas.corregidos;
            g_cola = (g_cola + 1) % kCapBuffer;
        }
    }

    void aplicarWaveRecta() {
        auto* p1 = PlayLayer::get() ? PlayLayer::get()->m_player1 : nullptr;
        if (!p1 || !p1->m_isDart || !g_activo || !bufferVacio()) return;
        if (!Mod::get()->getSettingValue<bool>("wave-recta")) return;
        
        constexpr float umbral = 0.05f;
        g_emitiendo = true;
        if (g_waveAngulo > umbral) PlayLayer::get()->handleButton(true, 1, true);
        else if (g_waveAngulo < -umbral) PlayLayer::get()->handleButton(false, 1, true);
        g_emitiendo = false;
    }

    void aplicarVueloRecto() {
        auto* p1 = PlayLayer::get() ? PlayLayer::get()->m_player1 : nullptr;
        if (!p1 || (!p1->m_isShip && !p1->m_isBird) || !g_activo || !bufferVacio()) return;
        if (!Mod::get()->getSettingValue<bool>("vuelo-recto")) return;

        float yActual = p1->getPositionY();
        if (!g_altitudFijada) { g_altitudObjetivo = yActual; g_altitudFijada = true; return; }
        
        float error = g_altitudObjetivo - yActual;
        float amort = static_cast<float>(Mod::get()->getSettingValue<double>("amortiguacion-vuelo"));
        float umbral = 5.f * (1.f - amort) + 2.f;

        g_emitiendo = true;
        if (error > umbral) PlayLayer::get()->handleButton(true, 1, true);
        else if (error < -umbral) PlayLayer::get()->handleButton(false, 1, true);
        g_emitiendo = false;
    }
}

// Movemos la logica del reloj a GJBaseGameLayer que es el verdadero motor del juego
class $modify(InputAssistBase, GJBaseGameLayer) {
    void update(float dt) {
        if (g_activo) {
            ++g_tickFisico;
            if (m_player1 && m_player1->m_isDart) g_waveAngulo = m_player1->m_yVelocity / 8.4f;
            procesarBuffer();
            aplicarWaveRecta();
            aplicarVueloRecto();
        }
        GJBaseGameLayer::update(dt);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (!g_activo || g_emitiendo) { 
            GJBaseGameLayer::handleButton(down, button, isPlayer1); 
            return; 
        }
        pushEntrada(down, button, isPlayer1);
    }
};

class $modify(InputAssistPlay, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        std::string modo = Mod::get()->getSettingValue<std::string>("modo-asistencia");
        g_activo = (modo != "desactivado");
        if (g_activo) {
            g_targetHz = 60.f; 
            recalcularTolerancia(); limpiarBuffer(); resetMetricas();
            g_tickFisico = 0; g_altitudFijada = false; g_waveAngulo = 0.f;
        }
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel(); limpiarBuffer(); g_tickFisico = 0; g_altitudFijada = false; g_waveAngulo = 0.f;
    }
};

$on_mod(Loaded) {
    listenForSettingChanges<int64_t>("tolerancia-ms", [](int64_t) { recalcularTolerancia(); });
    listenForSettingChanges<std::string>("modo-asistencia", [](std::string modo) {
        g_activo = (modo != "desactivado");
        if (!g_activo) limpiarBuffer();
    });
}
