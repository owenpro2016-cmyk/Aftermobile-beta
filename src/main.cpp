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
// 1. VENTANA DE OPCIONES DECORADA (POPUP)
// =====================================================================================

class AssistPopup : public geode::Popup<> {
protected:
    bool init() override {
        if (!Popup::init(280.f, 220.f, "GJ_square01.png")) return false;
        this->setTitle("Aftermobile Beta");

        auto* menu = CCMenu::create();
        menu->setPosition({0, 0});
        m_mainLayer->addChild(menu);

        auto* statusLabel = CCLabelBMFont::create("Estado del Mod:", "goldFont.fnt");
        statusLabel->setScale(0.7f);
        statusLabel->setPosition({140.f, 150.f});
        m_mainLayer->addChild(statusLabel);

        auto* toggleSpr = ButtonSprite::create("Alternar", "bigFont.fnt", "GJ_button_01.png", 0.8f);
        auto* toggleBtn = CCMenuItemSpriteExtra::create(
            toggleSpr, this, menu_selector(AssistPopup::onToggle)
        );
        toggleBtn->setPosition({140.f, 100.f});
        menu->addChild(toggleBtn);

        auto* resetSpr = ButtonSprite::create("Resetear", "bigFont.fnt", "GJ_button_04.png", 0.8f);
        auto* resetBtn = CCMenuItemSpriteExtra::create(
            resetSpr, this, menu_selector(AssistPopup::onReset)
        );
        resetBtn->setPosition({140.f, 50.f});
        menu->addChild(resetBtn);

        return true;
    }

    void onToggle(CCObject*) {
        std::string actual = Mod::get()->getSettingValue<std::string>("modo-asistencia");
        if (actual == "desactivado") {
            Mod::get()->setSettingValue<std::string>("modo-asistencia", "equilibrado");
            FLAlertLayer::create("Aftermobile", "Mod ACTIVADO.", "OK")->show();
        } else {
            Mod::get()->setSettingValue<std::string>("modo-asistencia", "desactivado");
            FLAlertLayer::create("Aftermobile", "Mod DESACTIVADO.", "OK")->show();
        }
    }

    void onReset(CCObject*) {
        FLAlertLayer::create("Aftermobile", "Datos reseteados.", "OK")->show();
    }

public:
    static AssistPopup* create() {
        auto* ret = new AssistPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// =====================================================================================
// 2. HUD CIRCULAR FLOTANTE
// =====================================================================================

class AssistHUD : public cocos2d::CCLayer {
public:
    cocos2d::CCSprite* m_circle = nullptr;
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

        m_circle = cocos2d::CCSprite::createWithSpriteFrameName("GJ_button_01.png");
        m_circle->setScale(0.8f);
        this->addChild(m_circle);

        m_label = cocos2d::CCLabelBMFont::create("AM", "bigFont.fnt");
        m_label->setScale(0.6f);
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
            std::string modo = Mod::get()->getSettingValue<std::string>("modo-asistencia");
            if (modo != "desactivado") {
                m_label->setColor({100, 255, 100});
            } else {
                m_label->setColor({150, 150, 150});
            }
        }
    }

    bool ccTouchBegan(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override {
        auto pos = this->convertTouchToNodeSpace(touch);
        if (pos.getLengthSq() < 625.f) return true;
        return false;
    }

    void ccTouchMoved(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override {
        auto delta = touch->getDelta();
        this->setPosition(this->getPosition() + delta);
    }

    void ccTouchEnded(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override {
        auto delta = touch->getDelta();
        if (std::abs(delta.x) < 5 && std::abs(delta.y) < 5) {
            AssistPopup::create()->show();
        }
    }

    void registerWithTouchDispatcher() override {
        cocos2d::CCDirector::get()->getTouchDispatcher()->addTargetedDelegate(this, -500, true);
    }
};

// =====================================================================================
// 3. VARIABLES GLOBALES Y LÓGICA DE FÍSICA (SISTEMA ARREGLADO)
// =====================================================================================

namespace {
    uint64_t g_tickFisico = 0;
    bool g_emitiendo = false;
    
    // Sistema simple: Solo guardamos si se presionó o soltó en este frame
    bool g_inputDown[2] = {false, false};
    bool g_inputReleased[2] = {false, false};
    bool g_isHeld[2] = {false, false};
    
    AssistHUD* g_hud = nullptr;

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
        
        for (int i = 0; i < 30; ++i) {
            x += vx;
            if (p1->m_isShip) vy += 0.4f;
            else if (p1->m_isBird) vy -= 0.958f;
            else vy -= 0.958f; 
            
            y += vy;
            drawNode->drawDot({x, y}, 2.5f, cocos2d::ccc4f(1.0f, 0.2f, 0.2f, 0.5f));
            if (y < 0) break;
        }
    }
}

// =====================================================================================
// 4. HOOKS DEL JUEGO
// =====================================================================================

class $modify(InputAssistBase, GJBaseGameLayer) {
    void update(float dt) {
        std::string modo = Mod::get()->getSettingValue<std::string>("modo-asistencia");
        
        if (modo != "desactivado") {
            ++g_tickFisico;
            
            // Procesar los inputs en el tick exacto de física
            for (int p = 0; p < 2; p++) {
                if (g_inputDown[p]) {
                    g_inputDown[p] = false;
                    g_isHeld[p] = true;
                    g_emitiendo = true;
                    GJBaseGameLayer::handleButton(true, 1, p == 0);
                    g_emitiendo = false;
                }
                if (g_inputReleased[p]) {
                    g_inputReleased[p] = false;
                    g_isHeld[p] = false;
                    g_emitiendo = true;
                    GJBaseGameLayer::handleButton(false, 1, p == 0);
                    g_emitiendo = false;
                }
            }
        }

        GJBaseGameLayer::update(dt);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        std::string modo = Mod::get()->getSettingValue<std::string>("modo-asistencia");
        
        // Si el mod está apagado o nosotros estamos emitiendo, pasa normal
        if (modo == "desactivado" || g_emitiendo) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        // Si el jugador tocó, guardamos el input para el próximo tick de física
        int p = isPlayer1 ? 0 : 1;
        if (down) g_inputDown[p] = true;
        else g_inputReleased[p] = true;
    }
};

class $modify(InputAssistPlay, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        
        g_tickFisico = 0;
        g_inputDown[0] = g_inputDown[1] = false;
        g_inputReleased[0] = g_inputReleased[1] = false;
        g_isHeld[0] = g_isHeld[1] = false;
        
        if (!g_hud) {
            g_hud = AssistHUD::create();
            this->addChild(g_hud, 9999);
        }

        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        g_tickFisico = 0;
        g_inputDown[0] = g_inputDown[1] = false;
        g_inputReleased[0] = g_inputReleased[1] = false;
        g_isHeld[0] = g_isHeld[1] = false;
    }

    void update(float dt) {
        std::string modo = Mod::get()->getSettingValue<std::string>("modo-asistencia");
        if (modo != "desactivado") {
            dibujarTrayectoria(this);
        }
        PlayLayer::update(dt);
    }
};

// =====================================================================================
// 5. BOTÓN EN EL MENÚ DE PAUSA
// =====================================================================================

class $modify(PauseLayerHook, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto* menu = getChildByID("right-button-menu");
        if (!menu) return;

        auto* spr = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
        auto* btn = CCMenuItemSpriteExtra::create(
            spr, this, menu_selector(PauseLayerHook::abrirPanel)
        );
        menu->addChild(btn);
    }

    void abrirPanel(CCObject*) {
        AssistPopup::create()->show();
    }
};

$on_mod(Loaded) {
    log::info("Aftermobile Beta: Sistema de input simple cargado.");
}
