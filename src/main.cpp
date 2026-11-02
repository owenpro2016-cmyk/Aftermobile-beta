#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <cocos2d.h>
#include <cmath>

using namespace geode::prelude;

// =====================================================================================
// 1. VENTANA DE OPCIONES (MENU GIGANTE CON PESTAÑAS)
// =====================================================================================

class AssistPopup : public geode::Popup {
protected:
    cocos2d::CCLayer* m_contentLayer = nullptr;
    cocos2d::CCMenu* m_tabMenu = nullptr;
    int m_currentTab = 0;

    void createToggle(cocos2d::CCLayer* parent, std::string settingID, std::string text, cocos2d::CCPoint pos) {
        auto* label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
        label->setScale(0.4f);
        label->setAnchorPoint({0.f, 0.5f});
        label->setPosition(pos);
        parent->addChild(label);

        auto* toggleSpr = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
        auto* toggleOnSpr = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        
        auto* toggler = CCMenuItemToggler::create(toggleSpr, toggleOnSpr, this, menu_selector(AssistPopup::onToggleSetting));
        toggler->setPosition({pos.x + 220.f, pos.y});
        
        // Usamos la ID del nodo para guardar qué ajuste estamos tocando
        toggler->setID(settingID);
        toggler->toggle(Mod::get()->getSettingValue<bool>(settingID));

        auto* menu = CCMenu::create();
        menu->setPosition({0, 0});
        menu->addChild(toggler);
        parent->addChild(menu);
    }

    void buildTab(int tab) {
        m_contentLayer->removeAllChildren();
        m_currentTab = tab;

        auto* scroll = ScrollLayer::create({260.f, 180.f});
        scroll->setPosition({10.f, 20.f});
        m_contentLayer->addChild(scroll);
        auto* content = scroll->m_contentLayer;

        float y = 160.f;
        if (tab == 0) { // Input
            createToggle(content, "cb-1", "Buffer Maximo", {10, y}); y -= 30;
            createToggle(content, "cb-2", "Tolerancia (ms)", {10, y}); y -= 30;
            createToggle(content, "cb-3", "Anti-Doble Click", {10, y}); y -= 30;
            createToggle(content, "cb-4", "Hold Delay", {10, y}); y -= 30;
        } else if (tab == 1) { // Trayectoria
            createToggle(content, "mostrar-trayectoria", "Mostrar Trayectoria", {10, y}); y -= 30;
            createToggle(content, "tr-1", "Longitud", {10, y}); y -= 30;
            createToggle(content, "tr-2", "Grosor", {10, y}); y -= 30;
            createToggle(content, "tr-3", "Opacidad", {10, y}); y -= 30;
        } else if (tab == 2) { // Wave/Ship
            createToggle(content, "wv-1", "Wave Recta", {10, y}); y -= 30;
            createToggle(content, "wv-2", "Fuerza Wave", {10, y}); y -= 30;
            createToggle(content, "sh-1", "Ship Altitud", {10, y}); y -= 30;
            createToggle(content, "sh-2", "Amortiguacion", {10, y}); y -= 30;
        } else if (tab == 3) { // Visuales
            createToggle(content, "mostrar-hud", "Mostrar Icono AM", {10, y}); y -= 30;
            createToggle(content, "vis-1", "Efecto Pulso", {10, y}); y -= 30;
            createToggle(content, "vis-2", "Destellos", {10, y}); y -= 30;
        }
    }

    void onToggleSetting(CCObject* sender) {
        auto* toggler = typeinfo_cast<CCMenuItemToggler*>(sender);
        if (!toggler) return;
        Mod::get()->setSettingValue<bool>(toggler->getID(), toggler->isToggled());
    }

    void onTab(CCObject* sender) {
        auto* btn = typeinfo_cast<CCMenuItemSpriteExtra*>(sender);
        if (btn) buildTab(btn->getTag());
    }

    bool init() override {
        if (!Popup::init(300.f, 280.f)) return false;
        this->setTitle("Aftermobile UI");

        auto* bg = cocos2d::CCLayerColor::create(cocos2d::ccc4(10, 20, 10, 255), 280.f, 240.f);
        bg->setPosition({10.f, 10.f});
        m_mainLayer->addChild(bg, -1);

        m_tabMenu = CCMenu::create();
        m_tabMenu->setPosition({0, 0});
        m_mainLayer->addChild(m_tabMenu);

        const char* tabs[] = {"Input", "Tray", "Fisica", "Vis"};
        for (int i = 0; i < 4; ++i) {
            auto* spr = ButtonSprite::create(tabs[i], "bigFont.fnt", "GJ_button_04.png", 0.4f);
            auto* btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(AssistPopup::onTab));
            btn->setPosition({35.f + i * 70.f, 230.f});
            btn->setTag(i);
            m_tabMenu->addChild(btn);
        }

        m_contentLayer = cocos2d::CCLayer::create();
        m_mainLayer->addChild(m_contentLayer);
        buildTab(0);

        return true;
    }

public:
    static AssistPopup* create() {
        auto* ret = new AssistPopup();
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret;
        return nullptr;
    }
};

// =====================================================================================
// 2. HUD CIRCULAR ANIMADO (COMO LA IMAGEN)
// =====================================================================================

class AssistHUD : public cocos2d::CCLayer {
public:
    cocos2d::CCDrawNode* m_circle = nullptr;
    cocos2d::CCLabelBMFont* m_label = nullptr;
    cocos2d::CCNode* m_sparkles = nullptr;

    static AssistHUD* create() {
        auto* ret = new AssistHUD();
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret;
        return nullptr;
    }

    bool init() override {
        if (!CCLayer::init()) return false;

        // 1. El círculo negro con borde verde (CORREGIDO: Orden de argumentos correcto)
        m_circle = cocos2d::CCDrawNode::create();
        // (Centro, Radio, ColorRelleno, GrosorBorde, ColorBorde, Segmentos)
        m_circle->drawCircle({0, 0}, 28.f, cocos2d::ccc4f(0.1f, 0.1f, 0.1f, 1.f), 3.f, cocos2d::ccc4f(0.f, 1.f, 0.f, 1.f), 32);
        this->addChild(m_circle);

        // 2. El texto "am" en verde
        m_label = cocos2d::CCLabelBMFont::create("am", "bigFont.fnt");
        m_label->setScale(0.9f);
        m_label->setColor({0, 255, 0});
        m_label->setPosition({0, -2});
        this->addChild(m_label);

        // 3. Los destellos (Sparkles) animados (CORREGIDO: Cast a float)
        m_sparkles = cocos2d::CCNode::create();
        for(int i=0; i<4; i++) {
            auto* dot = cocos2d::CCDrawNode::create();
            dot->drawDot({0, 0}, 2.f, cocos2d::ccc4f(1.f, 1.f, 1.f, 1.f));
            float angle = i * (static_cast<float>(M_PI) / 2.f);
            dot->setPosition({static_cast<float>(std::cos(angle)) * 20.f, static_cast<float>(std::sin(angle)) * 20.f});
            m_sparkles->addChild(dot);
        }
        m_sparkles->runAction(cocos2d::CCRepeatForever::create(cocos2d::CCRotateBy::create(2.0f, 360.f)));
        this->addChild(m_sparkles);

        this->setTouchEnabled(true);
        this->setTouchMode(cocos2d::kCCTouchesOneByOne);
        this->setTouchPriority(-500); 
        this->setPosition({50, cocos2d::CCDirector::get()->getWinSize().height - 100});
        this->scheduleUpdate();
        return true;
    }

    void update(float dt) override {
        std::string modo = Mod::get()->getSettingValue<std::string>("modo-asistencia");
        bool active = (modo != "desactivado");
        
        m_label->setColor(active ? cocos2d::ccColor3B(0, 255, 0) : cocos2d::ccColor3B(150, 150, 150));
        m_sparkles->setVisible(active && Mod::get()->getSettingValue<bool>("vis-2"));
        
        // CORREGIDO: Uso de Tag para controlar la animación
        bool showPulse = Mod::get()->getSettingValue<bool>("vis-1");
        if (showPulse) {
            if (!m_circle->getActionByTag(123)) {
                auto* pulse = cocos2d::CCRepeatForever::create(
                    cocos2d::CCSequence::create(
                        cocos2d::CCScaleTo::create(0.5f, 1.1f),
                        cocos2d::CCScaleTo::create(0.5f, 1.0f),
                        nullptr
                    )
                );
                pulse->setTag(123);
                m_circle->runAction(pulse);
            }
        } else {
            m_circle->stopActionByTag(123);
            m_circle->setScale(1.0f);
        }
    }

    bool ccTouchBegan(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override {
        auto pos = this->convertTouchToNodeSpace(touch);
        return pos.getLengthSq() < 900.f;
    }

    void ccTouchMoved(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override {
        this->setPosition(this->getPosition() + touch->getDelta());
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
// 3. MOTOR DE INPUT Y TRAYECTORIA (EL CEREBRO)
// =====================================================================================

namespace {
    uint64_t g_tickFisico = 0;
    bool g_emitiendo = false;
    
    bool g_inputDown[2] = {false, false};
    bool g_inputReleased[2] = {false, false};
    
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
        
        if (!Mod::get()->getSettingValue<bool>("mostrar-trayectoria")) return;

        auto* p1 = pl->m_player1;
        float x = p1->getPositionX();
        float y = p1->getPositionY();
        float vx = 8.4f; 
        float vy = p1->m_yVelocity;
        
        int maxTicks = Mod::get()->getSettingValue<int64_t>("tr-1");
        float thickness = Mod::get()->getSettingValue<double>("tr-2");
        float alpha = Mod::get()->getSettingValue<double>("tr-3");
        std::string color = Mod::get()->getSettingValue<std::string>("vis-3");
        
        cocos2d::ccColor4F col = (color == "verde") ? cocos2d::ccc4f(0,1,0,alpha) : 
                                 (color == "azul")  ? cocos2d::ccc4f(0,0,1,alpha) :
                                 (color == "amarillo") ? cocos2d::ccc4f(1,1,0,alpha) :
                                 cocos2d::ccc4f(1,0,0,alpha); // rojo por defecto

        for (int i = 0; i < maxTicks; ++i) {
            x += vx;
            if (p1->m_isShip) vy += 0.4f;
            else if (p1->m_isBird) vy -= 0.958f;
            else vy -= 0.958f; 
            
            y += vy;
            drawNode->drawDot({x, y}, thickness, col);
            if (y < 0) break;
        }
    }
}

class $modify(InputAssistBase, GJBaseGameLayer) {
    void update(float dt) {
        std::string modo = Mod::get()->getSettingValue<std::string>("modo-asistencia");
        
        if (modo != "desactivado") {
            ++g_tickFisico;
            
            for (int p = 0; p < 2; p++) {
                if (g_inputDown[p]) {
                    g_inputDown[p] = false;
                    g_emitiendo = true;
                    GJBaseGameLayer::handleButton(true, 1, p == 0);
                    g_emitiendo = false;
                }
                if (g_inputReleased[p]) {
                    g_inputReleased[p] = false;
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
        if (modo == "desactivado" || g_emitiendo) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

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
    }

    void update(float dt) {
        if (Mod::get()->getSettingValue<bool>("mostrar-trayectoria")) {
            dibujarTrayectoria(this);
        }
        PlayLayer::update(dt);
    }
};

class $modify(PauseLayerHook, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto* menu = getChildByID("right-button-menu");
        if (!menu) return;
        auto* spr = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
        auto* btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(PauseLayerHook::abrirPanel));
        menu->addChild(btn);
    }
    void abrirPanel(CCObject*) { AssistPopup::create()->show(); }
};
