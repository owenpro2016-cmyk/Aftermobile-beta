#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/binding/GameObject.hpp>
#include <cocos2d.h>

using namespace geode::prelude;

namespace {
    constexpr size_t kCapBuffer = 64;
    
    // Estructura para la cola de inputs sub-frame
    struct InputEvent {
        bool down;
        int button;
        bool isP1;
        uint64_t tick;
        double timestamp;
    };

    InputEvent g_queue[kCapBuffer]{};
    size_t g_head = 0, g_tail = 0;

    uint64_t g_tickFisico = 0;
    double g_currentTime = 0.0;
    
    // Variables para el anti-doble-click y hold inteligente
    double g_lastDownTime[2] = {0.0, 0.0}; // P1, P2
    bool g_pendingRelease[2] = {false, false};
    double g_pendingReleaseTime[2] = {0.0, 0.0};
    constexpr double kHoldDelay = 0.040; // 40ms de margen

    bool g_activo = false;
    bool g_emitiendo = false;

    inline bool queueLlena() { return ((g_head + 1) % kCapBuffer) == g_tail; }
    inline bool queueVacia() { return g_head == g_tail; }
    inline void limpiarQueue() { g_head = 0; g_tail = 0; }

    void pushEvent(bool down, int button, bool isP1) {
        if (queueLlena()) g_tail = (g_tail + 1) % kCapBuffer;
        g_queue[g_head] = InputEvent{ down, button, isP1, g_tickFisico, g_currentTime };
        g_head = (g_head + 1) % kCapBuffer;
    }

    // --- Lógica de trayectoria y dibujo ---
    void dibujarTrayectoria(PlayLayer* pl) {
        if (!pl || !pl->m_player1) return;
        
        // Buscamos o creamos el nodo de dibujo
        auto* drawNode = pl->getChildByID("trajectory-draw-node");
        if (!drawNode) {
            drawNode = cocos2d::CCDrawNode::create();
            drawNode->setID("trajectory-draw-node");
            pl->addChild(drawNode, 100);
        }
        
        drawNode->clear();
        if (!Mod::get()->getSettingValue<bool>("mostrar-hud")) return; // Usamos el setting del HUD para encender/apagar

        auto* p1 = pl->m_player1;
        float x = p1->getPositionX();
        float y = p1->getPositionY();
        float vx = 8.4f; // Velocidad base horizontal
        float vy = p1->m_yVelocity;
        
        bool isShip = p1->m_isShip;
        bool isBird = p1->m_isBird;
        
        // Simulamos 30 ticks de física hacia adelante
        for (int i = 0; i < 30; ++i) {
            x += vx;
            if (isShip) {
                vy += 0.4f; // Simulación simplificada de gravedad de nave
            } else if (isBird) {
                vy -= 0.958f;
            } else {
                vy -= 0.958f; // Gravedad normal
            }
            y += vy;

            // Dibujamos un punto en la trayectoria
            drawNode->drawDot({x, y}, 3.0f, cocos2d::ccc4f(1.0f, 0.2f, 0.2f, 0.6f));
            
            // Si cae de la pantalla, paramos
            if (y < 0) break;
        }
    }
}

class $modify(InputAssistBase, GJBaseGameLayer) {
    void update(float dt) {
        g_currentTime += dt;

        if (g_activo) {
            ++g_tickFisico;

            // 1. Procesar la cola de inputs
            while (!queueVacia()) {
                InputEvent* e = &g_queue[g_tail];
                
                // Si el tick ya pasó o es el actual
                if (e->tick <= g_tickFisico) {
                    // Aplicar anti-doble-click
                    if (e->down) {
                        if (g_currentTime - g_lastDownTime[e->isP1] < 0.030) { // 30ms
                            // Es un doble click accidental, lo ignoramos
                            g_tail = (g_tail + 1) % kCapBuffer;
                            continue; 
                        }
                        g_lastDownTime[e->isP1] = g_currentTime;
                        g_pendingRelease[e->isP1] = false; // Cancelar release pendiente
                    }

                    g_emitiendo = true;
                    GJBaseGameLayer::handleButton(e->down, e->button, e->isP1);
                    g_emitiendo = false;
                    
                    g_tail = (g_tail + 1) % kCapBuffer;
                } else {
                    break; // Aún no es el tick
                }
            }

            // 2. Procesar releases pendientes (Hold inteligente)
            for (int p = 0; p < 2; ++p) {
                if (g_pendingRelease[p]) {
                    if (g_currentTime >= g_pendingReleaseTime[p]) {
                        g_emitiendo = true;
                        GJBaseGameLayer::handleButton(false, 1, p == 0);
                        g_emitiendo = false;
                        g_pendingRelease[p] = false;
                    }
                }
            }
        }

        GJBaseGameLayer::update(dt);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (!g_activo || g_emitiendo) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        // Si estamos soltando el botón, no lo mandamos directo, lo agendamos
        // para ver si el jugador vuelve a pulsar rápido (spam).
        if (!down) {
            int p = isPlayer1 ? 0 : 1;
            g_pendingRelease[p] = true;
            g_pendingReleaseTime[p] = g_currentTime + kHoldDelay;
            // Lo guardamos en la cola pero marcado para el futuro
            pushEvent(down, button, isPlayer1);
        } else {
            pushEvent(down, button, isPlayer1);
        }
    }
};

class $modify(InputAssistPlay, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        
        std::string modo = Mod::get()->getSettingValue<std::string>("modo-asistencia");
        g_activo = (modo != "desactivado");
        
        if (g_activo) {
            g_tickFisico = 0;
            g_currentTime = 0.0;
            limpiarQueue();
            g_pendingRelease[0] = g_pendingRelease[1] = false;
            g_lastDownTime[0] = g_lastDownTime[1] = -1.0;
        }
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        g_tickFisico = 0;
        g_currentTime = 0.0;
        limpiarQueue();
        g_pendingRelease[0] = g_pendingRelease[1] = false;
    }

    void update(float dt) {
        // Dibujamos la trayectoria en cada frame
        if (g_activo) {
            dibujarTrayectoria(this);
        }
        PlayLayer::update(dt);
    }
};

$on_mod(Loaded) {
    listenForSettingChanges<std::string>("modo-asistencia", [](std::string modo) {
        g_activo = (modo != "desactivado");
        if (!g_activo) limpiarQueue();
    });
}
