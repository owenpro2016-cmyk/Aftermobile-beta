#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/ui/Popup.hpp>
#include <cocos2d.h>

using namespace geode::prelude;

// =====================================================================================
// 1. ESTRUCTURAS DE DATOS Y VARIABLES GLOBALES
// =====================================================================================

namespace {
    constexpr size_t kCapBuffer = 128;
    
    // Estructura para guardar cada input con su marca de tiempo exacta
    struct InputEvent {
        bool down;
        int button;
        bool isP1;
        uint64_t tick;
        double timestamp;
    };

    // Cola circular para no usar memoria dinámica en el bucle de física
    InputEvent g_queue[kCapBuffer]{};
    size_t g_head = 0, g_tail = 0;

    uint64_t g_tickFisico = 0;
    double g_currentTime = 0.0;
    
    // Variables de anti-doble click y hold inteligente
    double g_lastDownTime[2] = {0.0, 0.0};
    bool g_pendingRelease[2] = {false, false};
    double g_pendingReleaseTime[2] = {0.0, 0.0};
    constexpr double kHoldDelay = 0.040; // 40ms para detectar spam

    bool g_activo = false;
    bool g_emitiendo = false;
    
    // Referencia global al HUD
    class AssistHUD* g_hud = nullptr;
}

// =====================================================================================
// 2. CLASE DEL HUD FLOTANTE (EL BOTÓN "AM")
// =====================================================================================

class AssistHUD : public cocos2d::CCLayer {
public:
    static AssistHUD* create() {
        auto* ret = new AssistHUD();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    bool init() override {
        if (!CCLayer::init()) return false;

        // Creamos el fondo del botón (un círculo o cuadrado semi-transparente)
        auto* bg = cocos2d::CCLayerColor::create(cocos2d::ccc4(30, 30, 30, 200), 50, 50);
        bg->setPosition({-25, -25}); // Centrado
        this->addChild(bg);

        // Texto "AM"
        auto* label = cocos2d::CCLabelBMFont::create("AM", "bigFont.fnt");
        label->setScale(0.6f);
        label->setPosition({0, 0});
        this->addChild(label);

        // Para hacerlo arrastrable, necesitamos registrar los toques
        this->setTouchEnabled(true);
        this->setTouchMode(cocos2d::kCCTouchesOneByOne);
        this->setTouchPriority(-500); // Prioridad alta para que no lo tape el juego

        // Lo ponemos en una posición inicial (arriba a la izquierda)
        this->setPosition({50, cocos2d::CCDirector::get()->getWinSize().height - 100});

        // Actualizar estado visual cada frame
        this->scheduleUpdate();
        return true;
    }

    void update(float dt) override {
        // Si el mod está desactivado, ponemos el texto grisáceo
        if (auto* label = typeinfo_cast<cocos2d::CCLabelBMFont*>(this->getChildren()->objectAtIndex(1))) {
            if (!g_activo) {
                label->setColor({150, 150, 150});
            } else {
                label->setColor({255, 255, 255});
            }
        }
    }

    // --- Lógica para arrastrar el HUD ---
    bool ccTouchBegan(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override {
        auto pos = this->convertTouchToNodeSpace(touch);
        cocos2d::CCRect rect(-25, -25, 50, 50);
        return rect.containsPoint(pos);
    }

    void ccTouchMoved(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override {
        auto delta = touch->getDelta();
        this->setPosition(this->getPosition() + delta);
    }

    void ccTouchEnded(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override {
        // Si fue un toque rápido (no arrastre), abrimos el menú
        auto delta = touch->getDelta();
        if (std::abs(delta.x) < 5 && std::abs(delta.y) < 5) {
            this->abrirMenu();
        }
    }

    void abrirMenu() {
        // Aquí abriremos el Popup más adelante
        // AssistPanel::create()->show();
    }

    void registerWithTouchDispatcher() override {
        cocos2d::CCDirector::get()->getTouchDispatcher()->addTargetedDelegate(this, -500, true);
    }
};


// =====================================================================================
// 3. LÓGICA DE LA COLA DE INPUTS Y TRAYECTORIA
// =====================================================================================

namespace {
    inline bool queueLlena() { return ((g_head + 1) % kCapBuffer) == g_tail; }
    inline bool queueVacia() { return g_head == g_tail; }
    inline void limpiarQueue() { g_head = 0; g_tail = 0; }

    void pushEvent(bool down, int button, bool isP1) {
        if (queueLlena()) g_tail = (g_tail + 1) % kCapBuffer;
        g_queue[g_head] = InputEvent{ down, button, isP1, g_tickFisico, g_currentTime };
        g_head = (g_head + 1) % kCapBuffer;
    }

    // Dibuja la trayectoria usando CCDrawNode
    void dibujarTrayectoria(PlayLayer* pl) {
        if (!pl || !pl->m_player1) return;
        
        auto* drawNode = typeinfo_cast<cocos2d::CCDrawNode*>(pl->getChildByID("trajectory-draw-node"));
        if (!drawNode) {
            drawNode = cocos2d::CCDrawNode::create();
            drawNode->setID("trajectory-draw-node");
            pl->addChild(drawNode, 100);
        }
        
        drawNode->clear();
        
        // Solo dibujamos si el mod está activo
        if (!g_activo) return;

        auto* p1 = pl->m_player1;
        float x = p1->getPositionX();
        float y = p1->getPositionY();
        float vx = 8.4f; 
        float vy = p1->m_yVelocity;
        
        bool isShip = p1->m_isShip;
        bool isBird = p1->m_isBird;
        
        // Simulamos 30 ticks
        for (int i = 0; i < 30; ++i) {
            x += vx;
            if (isShip) {
                vy += 0.4f;
            } else if (isBird) {
                vy -= 0.958f;
            } else {
                vy -= 0.958f; 
            }
            y += vy;

            // Dibujamos el punto de predicción
            drawNode->drawDot({x, y}, 2.5f, cocos2d::ccc4f(1.0f, 0.2f, 0.2f, 0.5f));
            
            if (y < 0) break;
        }
    }
}

// =====================================================================================
// 4. HOOKS DEL JUEGO (LÓGICA CENTRAL)
// =====================================================================================

class $modify(InputAssistBase, GJBaseGameLayer) {
    void update(float dt) {
        g_currentTime += dt;

        if (g_activo) {
            ++g_tickFisico;

            // 1. Procesar cola de inputs
            while (!queueVacia()) {
                InputEvent* e = &g_queue[g_tail];
                
                if (e->tick <= g_tickFisico) {
                    // Anti-doble-click: si bajó el botón muy rápido, lo ignoramos
                    if (e->down) {
                        if (g_currentTime - g_lastDownTime[e->isP1] < 0.030) { 
                            g_tail = (g_tail + 1) % kCapBuffer;
                            continue; 
                        }
                        g_lastDownTime[e->isP1] = g_currentTime;
                        g_pendingRelease[e->isP1] = false; // Cancelamos release pendiente
                    }

                    g_emitiendo = true;
                    GJBaseGameLayer::handleButton(e->down, e->button, e->isP1);
                    g_emitiendo = false;
                    
                    g_tail = (g_tail + 1) % kCapBuffer;
                } else {
                    break; // Aún no es el tick
                }
            }

            // 2. Procesar Hold Inteligente (evitar micro-releases)
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

        // Si está soltando, agendamos el release para ver si vuelve a pulsar rápido
        if (!down) {
            int p = isPlayer1 ? 0 : 1;
            g_pendingRelease[p] = true;
            g_pendingReleaseTime[p] = g_currentTime + kHoldDelay;
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
        
        // Creamos el HUD
        if (!g_hud) {
            g_hud = AssistHUD::create();
            this->addChild(g_hud, 9999);
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
        if (g_activo) {
            dibujarTrayectoria(this);
        }
        PlayLayer::update(dt);
    }
};

// =====================================================================================
// 5. CICLO DE VIDA DEL MOD
// =====================================================================================

$on_mod(Loaded) {
    listenForSettingChanges<std::string>("modo-asistencia", [](std::string modo) {
        g_activo = (modo != "desactivado");
        if (!g_activo) limpiarQueue();
    });
}
