#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/ui/Popup.hpp>
#include <cocos2d.h>
#include <cmath>

using namespace geode::prelude;

// =====================================================================================
// 1. CLASE DEL HUD FLOTANTE (EL BOTÓN "AM")
// =====================================================================================

class AssistHUD : public cocos2d::CCLayer {
public:
    cocos2d::CCLabelBMFont* m_label = nullptr;

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

        auto* bg = cocos2d::CCLayerColor::create(cocos2d::ccc4(30, 30, 30, 200), 50, 50);
        bg->setPosition({-25, -25});
        this->addChild(bg);

        m_label = cocos2d::CCLabelBMFont::create("AM", "bigFont.fnt");
        m_label->setScale(0.6f);
        m_label->setPosition({0, 0});
        this->addChild(m_label);

        this->setTouchEnabled(true);
        this->setTouchMode(cocos2d::kCCTouchesOneByOne);
        this->setTouchPriority(-500); 

        this->setPosition({50, cocos2d::CCDirector::get()->getWinSize().height - 100});
        this->scheduleUpdate();
        return true;
    }

    void update(float dt) override {
        if (m_label) {
            // CORREGIDO: Ahora lee bien el string y no intenta usar .empty() en un bool
            std::string modo = Mod::get()->getSettingValue<std::string>("modo-asistencia");
            if (modo != "desactivado") {
                m_label->setColor({100, 255, 100}); // Verde si está activo
            } else {
                m_label->setColor({150, 150, 150}); // Gris si está off
            }
        }
    }

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
        auto delta = touch->getDelta();
        if (std::abs(delta.x) < 5 && std::abs(delta.y) < 5) {
            FLAlertLayer::create("Aftermobile", "El menu flotante estara aqui pronto!", "OK")->show();
        }
    }

    void registerWithTouchDispatcher() override {
        cocos2d::CCDirector::get()->getTouchDispatcher()->addTargetedDelegate(this, -500, true);
    }
};

// =====================================================================================
// 2. VARIABLES GLOBALES Y LÓGICA DE INPUT
// =====================================================================================

namespace {
    constexpr size_t kCapBuffer = 128;
    
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
    
    double g_lastDownTime[2] = {0.0, 0.0};
    bool g_pendingRelease[2] = {false, false};
    double g_pendingReleaseTime[2] = {0.0, 0.0};
    constexpr double kHoldDelay = 0.040; 

    bool g_emitiendo = false;
    
    AssistHUD* g_hud = nullptr;

    inline bool queueLlena() { return ((g_head + 1) % kCapBuffer) == g_tail; }
    inline bool queueVacia() { return g_head == g_tail; }
    inline void limpiarQueue() { g_head = 0; g_tail = 0; }

    void pushEvent(bool down, int button, bool isP1) {
        if (queueLlena()) g_tail = (g_tail + 1) % kCapBuffer;
        g_queue[g_head] = InputEvent{ down, button, isP1, g_tickFisico, g_currentTime };
        g_head = (g_head + 1) % kCapBuffer;
    }

    void dibujarTrayectoria(PlayLayer* pl) {
        if (!pl || !pl->m_player1) return;
        
        auto* drawNode = typeinfo_cast<cocos2d::CCDrawNode*>(pl->getChildByID("trajectory-draw-node"));
        if (!drawNode) {
            drawNode = cocos2d::CCDrawNode::create();
            drawNode->setID("trajectory-draw-node");
            pl->addChild(drawNode, 100);
        }
        
        drawNode->clear();
        
        std::string modo = Mod::get()->getSettingValue<std::string>("modo-asistencia");
        if (modo == "desactivado") return;

        auto* p1 = pl->m_player1;
        float x = p1->getPositionX();
        float y = p1->getPositionY();
        float vx = 8.4f; 
        float vy = p1->m_yVelocity;
        
        bool isShip = p1->m_isShip;
        bool isBird = p1->m_isBird;
        
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

            drawNode->drawDot({x, y}, 2.5f, cocos2d::ccc4f(1.0f, 0.2f, 0.2f, 0.5f));
            
            if (y < 0) break;
        }
    }
}

// =====================================================================================
// 3. HOOKS DEL JUEGO
// =====================================================================================

class $modify(InputAssistBase, GJBaseGameLayer) {
    void update(float dt) {
        g_currentTime += dt;

        std::string modo = Mod::get()->getSettingValue<std::string>("modo-asistencia");
        bool activo = (modo != "desactivado");

        if (activo) {
            ++g_tickFisico;

            while (!queueVacia()) {
                InputEvent* e = &g_queue[g_tail];
                
                if (e->tick <= g_tickFisico) {
                    if (e->down) {
                        if (g_currentTime - g_lastDownTime[e->isP1] < 0.030) { 
                            g_tail = (g_tail + 1) % kCapBuffer;
                            continue; 
                        }
                        g_lastDownTime[e->isP1] = g_currentTime;
                        g_pendingRelease[e->isP1] = false;
                    }

                    g_emitiendo = true;
                    GJBaseGameLayer::handleButton(e->down, e->button, e->isP1);
                    g_emitiendo = false;
                    
                    g_tail = (g_tail + 1) % kCapBuffer;
                } else {
                    break;
                }
            }

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
        std::string modo = Mod::get()->getSettingValue<std::string>("modo-asistencia");
        
        if (modo == "desactivado" || g_emitiendo) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

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
        
        if (modo != "desactivado") {
            g_tickFisico = 0;
            g_currentTime = 0.0;
            limpiarQueue();
            g_pendingRelease[0] = g_pendingRelease[1] = false;
            g_lastDownTime[0] = g_lastDownTime[1] = -1.0;
        }
        
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
        std::string modo = Mod::get()->getSettingValue<std::string>("modo-asistencia");
        if (modo != "desactivado") {
            dibujarTrayectoria(this);
        }
        PlayLayer::update(dt);
    }
};

$on_mod(Loaded) {
    log::info("Aftermobile Beta cargado correctamente.");
}
