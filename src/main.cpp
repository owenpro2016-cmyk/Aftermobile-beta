#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <cocos2d.h>
#include <cmath>
#include <vector>
#include <deque>
#include <map>
#include <array>
#include <algorithm>

using namespace geode::prelude;

// =====================================================================================
// 1. MOTOR MAESTRO (AfterEngine Fusion)
// =====================================================================================

namespace AfterEngine {
    // --- CONSTANTES FÍSICAS EXACTAS ---
    constexpr float GRAVITY = 0.958199f;
    constexpr float JUMP_VELOCITY = 11.180032f;
    constexpr float TERMINAL_VELOCITY = 15.5f;
    constexpr float PLAYER_SIZE = 30.0f;
    constexpr float TICK_RATE = 240.0f; // GD corre a 240 TPS internamente

    // --- ESTRUCTURAS DE DATOS ---
    struct Hitbox {
        float minX, minY, maxX, maxY;
        bool isHazard;
        bool isOrb;
        bool isPad;
        bool isPortal;
        int objectID;
    };

    struct PlayerState {
        float x, y, vY;
        bool onGround;
        uint64_t tick;
        bool isShip;
        bool isWave;
        bool isUFO;
        bool isBall;
    };

    struct InputLog {
        uint64_t tick;
        bool down;
        int player;
    };

    // --- VARIABLES GLOBALES ---
    std::array<Hitbox, 512> g_cachedHitboxes; // Cache masivo para niveles densos
    size_t g_hitboxCount = 0;

    std::deque<PlayerState> g_playerHistory; // Historial para CAF (Rewind)
    constexpr size_t MAX_HISTORY = 10;

    std::vector<InputLog> g_replayData;
    size_t g_replayIndex = 0;

    // Memoria de Nivel: Mapea coordenada X a un delay exitoso
    std::map<int, int> g_levelMemory;

    uint64_t g_currentTick = 0;
    bool g_holding[2] = {false, false};
    bool g_bufferedJump[2] = {false, false};
    uint64_t g_jumpStart[2] = {0, 0};
    bool g_usedRewind[2] = {false, false};

    // Contadores para Spam Guard
    int g_spamCounter[2] = {0, 0};

    // --- OBTENER TICKS SEGÚN MODO ---
    int getMaxTicks(PlayerObject* p) {
        std::string mode = Mod::get()->getSettingValue<std::string>("assist-mode");
        if (mode == "Desactivado") return 0;
        
        bool isWave = p->m_isDart;
        bool isShip = p->m_isShip;
        
        if (mode == "Custom") {
            if (isWave) return Mod::get()->getSettingValue<int64_t>("custom-wave-ticks");
            if (isShip) return 4;
            return Mod::get()->getSettingValue<int64_t>("custom-ticks");
        }
        
        // Presets
        if (mode == "Suave") return isWave ? 2 : 6;
        if (mode == "Balanceado") return isWave ? 4 : 12;
        if (mode == "Extremo") return isWave ? 8 : 24;
        return 12;
    }

    // --- ESCANEO DE OBJETOS (Hitboxes, Orbes, Portales) ---
    void updateCache(PlayLayer* pl, PlayerObject* p1) {
        g_hitboxCount = 0;
        if (!pl || !p1 || !pl->m_objects) return;
        float pX = p1->getPositionX(), pY = p1->getPositionY();

        for (auto* obj : CCArrayExt<GameObject*>(pl->m_objects)) {
            if (g_hitboxCount >= g_cachedHitboxes.size()) break;
            if (!obj) continue;
            
            auto type = obj->m_objectType;
            float oX = obj->getPositionX(), oY = obj->getPositionY();
            
            // Escaneo en un radio de 800px para predecir trayectorias largas
            if (oX > pX - 200.f && oX < pX + 800.f && oY > pY - 400.f && oY < pY + 400.f) {
                bool isHazard = (type == GameObjectType::Hazard);
                bool isSolid = (type == GameObjectType::Solid);
                bool isOrb = (type == GameObjectType::Special && obj->m_objectID >= 17 && obj->m_objectID <= 24);
                bool isPad = (type == GameObjectType::Special && obj->m_objectID >= 35 && obj->m_objectID <= 41);
                bool isPortal = (type == GameObjectType::Portal); // Simplificado

                if (isHazard || isSolid || isOrb || isPad || isPortal) {
                    auto rect = obj->getObjectRect();
                    g_cachedHitboxes[g_hitboxCount++] = { 
                        rect.getMinX(), rect.getMinY(), rect.getMaxX(), rect.getMaxY(), 
                        isHazard, isOrb, isPad, isPortal, obj->m_objectID 
                    };
                }
            }
        }
    }

    // --- SIMULADOR DE COLISIÓN PREDICTIVA ---
    bool checkCollision(float x, float y, bool ignoreOrbs = true) {
        float half = PLAYER_SIZE / 2.0f;
        for (size_t i = 0; i < g_hitboxCount; i++) {
            const auto& hb = g_cachedHitboxes[i];
            if (ignoreOrbs && (hb.isOrb || hb.isPortal || hb.isPad)) continue;
            
            // Hitbox de GD ajustada (-2px de margen para simular los triggers del juego)
            if (x + half - 2.f > hb.minX && x - half + 2.f < hb.maxX && 
                y + half - 2.f > hb.minY && y - half + 2.f < hb.maxY) {
                return true;
            }
        }
        return false;
    }

    // --- PREDICCIÓN DE TRAYECTORIA CON ORBES Y PORTALES ---
    bool isJumpSafeFrom(float startX, float startY, PlayerObject* p1) {
        float pX = startX, pY = startY;
        float vX = 8.4f * (p1->m_speedMultiplier > 0 ? p1->m_speedMultiplier : 1.0f);
        float vY = JUMP_VELOCITY;
        
        bool isShip = p1->m_isShip;
        bool isWave = p1->m_isDart;
        bool isUFO = p1->m_isBird;
        bool isBall = p1->m_isBall;

        for (int t = 1; t <= 40; ++t) {
            float nextX = pX + (vX * t);
            float nextY = pY + (vY * t) - (0.5f * GRAVITY * t * t);
            
            // Si estamos simulando un salto, vY se ve afectada
            if (isShip) vY += 0.4f; 
            
            if (checkCollision(nextX, nextY)) return false;

            // Si pasa por un orbe o pad y "smart-prediction" está activo, simula saltar.
            if (Mod::get()->getSettingValue<bool>("smart-prediction")) {
                for (size_t i = 0; i < g_hitboxCount; i++) {
                    const auto& hb = g_cachedHitboxes[i];
                    if (hb.isOrb || hb.isPad) {
                        if (nextX + 15 > hb.minX && nextX - 15 < hb.maxX && nextY + 15 > hb.minY && nextY - 15 < hb.maxY) {
                            vY = (hb.isPad) ? 16.0f : JUMP_VELOCITY; // Fuerza de orbe o pad
                        }
                    }
                }
            }

            // Fin de la predicción si cae por debajo
            if (nextY <= pY && t > 5 && !isWave && !isShip) break; 
        }
        return true;
    }

    // --- CLICK AFTER FRAMES (REWIND) ---
    void attemptLateClickRewind(PlayerObject* p1, int p) {
        if (!Mod::get()->getSettingValue<bool>("caf-enabled") || g_playerHistory.empty()) return;
        
        int maxRewind = Mod::get()->getSettingValue<int64_t>("caf-max-rewind");
        int rewindCount = std::min((int)g_playerHistory.size(), maxRewind);

        for (int i = 1; i <= rewindCount; i++) {
            PlayerState past = g_playerHistory[g_playerHistory.size() - i];
            
            // Si en el pasado estaba en el suelo y saltar desde ahi es seguro...
            if (past.onGround && isJumpSafeFrom(past.x, past.y, p1)) {
                p1->setPositionX(past.x); 
                p1->setPositionY(past.y);
                p1->m_yVelocity = JUMP_VELOCITY;
                p1->m_isOnGround = false;
                g_usedRewind[p] = true;
                log::debug("CAF: Rewind aplicado ({} ticks atras).", i);
                return;
            }
        }
    }
}

// =====================================================================================
// 2. UI: HUD Y PANEL DE CONTROL (Afterflow/Nova Style)
// =====================================================================================

class AfterPanel : public geode::Popup {
protected:
    bool init() override {
        if (!Popup::init(320.f, 260.f)) return false;
        this->setTitle("Aftermobile Ultimate");

        auto colSurface = Mod::get()->getSettingValue<cocos2d::ccColor3B>("ui-surface-color");
        auto colPrimary = Mod::get()->getSettingValue<cocos2d::ccColor3B>("ui-primary-color");

        auto* bg = cocos2d::CCLayerColor::create(cocos2d::ccc4(colSurface.r, colSurface.g, colSurface.b, 255), 300.f, 210.f);
        bg->setPosition({10.f, 10.f});
        m_mainLayer->addChild(bg, -1);

        auto* title = CCLabelBMFont::create("ULTIMATE ENGINE", "bigFont.fnt");
        title->setColor(colPrimary);
        title->setScale(0.6f);
        title->setPosition({160.f, 195.f});
        m_mainLayer->addChild(title);

        auto* menu = CCMenu::create();
        menu->setPosition({0, 0});
        m_mainLayer->addChild(menu);

        // Boton para alternar el modo de asistencia
        auto* toggleSpr = ButtonSprite::create("Cycle Assist Mode", "bigFont.fnt", "GJ_button_01.png", 0.6f);
        auto* toggleBtn = CCMenuItemSpriteExtra::create(toggleSpr, this, menu_selector(AfterPanel::onCycleMode));
        toggleBtn->setPosition({160.f, 140.f});
        menu->addChild(toggleBtn);

        // Boton para ver el HUD
        auto* hudSpr = ButtonSprite::create("Toggle Hitboxes", "bigFont.fnt", "GJ_button_04.png", 0.6f);
        auto* hudBtn = CCMenuItemSpriteExtra::create(hudSpr, this, menu_selector(AfterPanel::onToggleHitboxes));
        hudBtn->setPosition({160.f, 85.f});
        menu->addChild(hudBtn);

        // Boton para limpiar memoria
        auto* clrSpr = ButtonSprite::create("Clear Level Memory", "bigFont.fnt", "GJ_button_06.png", 0.6f);
        auto* clrBtn = CCMenuItemSpriteExtra::create(clrSpr, this, menu_selector(AfterPanel::onClearMemory));
        clrBtn->setPosition({160.f, 30.f});
        menu->addChild(clrBtn);

        return true;
    }

    void onCycleMode(CCObject*) {
        std::string mode = Mod::get()->getSettingValue<std::string>("assist-mode");
        if (mode == "Desactivado") Mod::get()->setSettingValue<std::string>("assist-mode", "Suave");
        else if (mode == "Suave") Mod::get()->setSettingValue<std::string>("assist-mode", "Balanceado");
        else if (mode == "Balanceado") Mod::get()->setSettingValue<std::string>("assist-mode", "Extremo");
        else Mod::get()->setSettingValue<std::string>("assist-mode", "Desactivado");
        
        FLAlertLayer::create("Modo Cambiado", ("Ahora: " + Mod::get()->getSettingValue<std::string>("assist-mode")).c_str(), "OK")->show();
    }

    void onToggleHitboxes(CCObject*) {
        bool hb = Mod::get()->getSettingValue<bool>("show-hitboxes");
        Mod::get()->setSettingValue<bool>("show-hitboxes", !hb);
    }

    void onClearMemory(CCObject*) {
        AfterEngine::g_levelMemory.clear();
        FLAlertLayer::create("Memoria", "Memoria de nivel borrada.", "OK")->show();
    }

public:
    static AfterPanel* create() {
        auto* ret = new AfterPanel();
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret; return nullptr;
    }
};

class AfterHUD : public cocos2d::CCLayer {
public:
    static AfterHUD* create() {
        auto* ret = new AfterHUD();
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret; return nullptr;
    }
    bool init() override {
        if (!CCLayer::init()) return false;
        auto colPrimary = Mod::get()->getSettingValue<cocos2d::ccColor3B>("ui-primary-color");
        auto c4 = cocos2d::ccc4f(colPrimary.r / 255.f, colPrimary.g / 255.f, colPrimary.b / 255.f, 1.f);
        
        auto* circle = cocos2d::CCDrawNode::create();
        circle->drawCircle({0, 0}, 22.f, cocos2d::ccc4f(0.1f, 0.1f, 0.1f, 0.9f), 2.f, c4, 32);
        circle->setID("am-circle");
        this->addChild(circle);
        
        auto* label = cocos2d::CCLabelBMFont::create("am", "bigFont.fnt");
        label->setScale(0.7f); label->setColor(colPrimary); this->addChild(label);
        
        this->setTouchEnabled(true); this->setTouchMode(cocos2d::kCCTouchesOneByOne);
        this->setTouchPriority(-500); this->setPosition({50, cocos2d::CCDirector::get()->getWinSize().height - 100});
        return true;
    }
    bool ccTouchBegan(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override {
        return this->convertTouchToNodeSpace(touch).getLengthSq() < 400.f;
    }
    void ccTouchMoved(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override {
        this->setPosition(this->getPosition() + touch->getDelta());
    }
    void ccTouchEnded(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override {
        auto delta = touch->getDelta();
        if (std::abs(delta.x) < 5 && std::abs(delta.y) < 5) AfterPanel::create()->show();
    }
    void registerWithTouchDispatcher() override {
        cocos2d::CCDirector::get()->getTouchDispatcher()->addTargetedDelegate(this, -500, true);
    }
};

// =====================================================================================
// 3. INTERCEPCIÓN DE INPUTS (CBF, CAF, Buffer)
// =====================================================================================

class $modify(PlayerObjectHook, PlayerObject) {
    void pushButton(PlayerButton button) {
        if (button != PlayerButton::Jump) { PlayerObject::pushButton(button); return; }
        
        auto* pl = PlayLayer::get();
        if (!pl) return;
        int p = (this == pl->m_player1) ? 0 : 1;
        std::string mode = Mod::get()->getSettingValue<std::string>("assist-mode");

        AfterEngine::g_holding[p] = true;

        if (this->m_isOnGround) {
            // 1. Logica de Click After Frames (CAF)
            if (mode == "Desactivado") {
                AfterEngine::attemptLateClickRewind(this, p);
            } 
            // 2. Logica de Buffer Temprano (TickSaver/Nova)
            else {
                AfterEngine::g_bufferedJump[p] = true;
                AfterEngine::g_jumpStart[p] = AfterEngine::g_currentTick;
                AfterEngine::g_usedRewind[p] = false;
            }
        } else {
            PlayerObject::pushButton(button);
        }
    }

    void releaseButton(PlayerButton button) {
        if (button != PlayerButton::Jump) { PlayerObject::releaseButton(button); return; }
        auto* pl = PlayLayer::get();
        if (!pl) return;
        int p = (this == pl->m_player1) ? 0 : 1;
        
        AfterEngine::g_holding[p] = false;
        if (AfterEngine::g_bufferedJump[p]) { 
            AfterEngine::g_bufferedJump[p] = false; 
            return; 
        }
        PlayerObject::releaseButton(button);
    }
};

class $modify(InputAssistBase, GJBaseGameLayer) {
    void update(float dt) {
        AfterEngine::g_currentTick++;
        auto* pl = PlayLayer::get();
        if (!pl || !pl->m_player1) { GJBaseGameLayer::update(dt); return; }

        // 1. GUARDAR HISTORIAL PARA CAF
        AfterEngine::g_playerHistory.push_back({
            pl->m_player1->getPositionX(), pl->m_player1->getPositionY(),
            pl->m_player1->m_yVelocity, pl->m_player1->m_isOnGround,
            AfterEngine::g_currentTick, pl->m_player1->m_isShip,
            pl->m_player1->m_isDart, pl->m_player1->m_isBird, pl->m_player1->m_isBall
        });
        if (AfterEngine::g_playerHistory.size() > AfterEngine::MAX_HISTORY) {
            AfterEngine::g_playerHistory.pop_front();
        }

        // 2. LOGICA DEL BUFFER INTELIGENTE
        if (Mod::get()->getSettingValue<std::string>("assist-mode") != "Desactivado") {
            AfterEngine::updateCache(pl, pl->m_player1);

            for (int i = 0; i < 2; i++) {
                PlayerObject* player = (i == 0) ? pl->m_player1 : pl->m_player2;
                if (!player) continue;

                if (AfterEngine::g_bufferedJump[i]) {
                    int maxTicks = AfterEngine::getMaxTicks(player);
                    uint64_t elapsed = AfterEngine::g_currentTick - AfterEngine::g_jumpStart[i];

                    if (player->m_isOnGround) {
                        // Si es seguro, o ya paso el tiempo limite, saltamos
                        bool safe = AfterEngine::isJumpSafeFrom(player->getPositionX(), player->getPositionY(), player);
                        
                        if (safe || elapsed >= maxTicks) {
                            player->pushButton(PlayerButton::Jump);
                            AfterEngine::g_bufferedJump[i] = false;
                            
                            // Registrar exito en la memoria de nivel
                            if (Mod::get()->getSettingValue<bool>("level-memory")) {
                                AfterEngine::g_levelMemory[(int)(player->getPositionX() / 30)] = elapsed;
                            }

                            if (!AfterEngine::g_holding[i]) player->releaseButton(PlayerButton::Jump);
                        }
                    } else {
                        AfterEngine::g_bufferedJump[i] = false;
                    }
                }
            }
        }
        GJBaseGameLayer::update(dt);
    }
};

// =====================================================================================
// 4. HOOKS DE UI Y DIBUJO
// =====================================================================================

class $modify(MenuLayerHook, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        if (Mod::get()->getSettingValue<bool>("show-hud")) this->addChild(AfterHUD::create(), 9999);
        return true;
    }
};

class $modify(PauseLayerHookUI, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto* menu = getChildByID("right-button-menu");
        if (!menu) return;
        auto* btn = CCMenuItemSpriteExtra::create(CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png"), this, menu_selector(PauseLayerHookUI::abrirPanel));
        menu->addChild(btn);
    }
    void abrirPanel(CCObject*) { AfterPanel::create()->show(); }
};

class $modify(InputAssistPlay, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        AfterEngine::g_playerHistory.clear();
        AfterEngine::g_replayIndex = 0;
        if (Mod::get()->getSettingValue<bool>("show-hud")) this->addChild(AfterHUD::create(), 9999);
        return true;
    }

    void draw() {
        PlayLayer::draw();
        if (!m_player1) return;
        
        auto* drawNode = typeinfo_cast<cocos2d::CCDrawNode*>(this->getChildByID("aero-draw-node"));
        if (!drawNode) {
            drawNode = cocos2d::CCDrawNode::create();
            drawNode->setID("aero-draw-node");
            this->addChild(drawNode, 100);
        }
        drawNode->clear();

        // 1. Dibujar Hitboxes
        if (Mod::get()->getSettingValue<bool>("show-hitboxes")) {
            for (size_t i = 0; i < AfterEngine::g_hitboxCount; i++) {
                auto& hb = AfterEngine::g_cachedHitboxes[i];
                if (hb.isPortal || hb.isOrb) continue;
                
                cocos2d::ccColor4F color = hb.isHazard ? cocos2d::ccc4f(1, 0, 0, 0.5f) : cocos2d::ccc4f(0, 1, 0, 0.5f);
                cocos2d::CCRect rect(hb.minX, hb.minY, hb.maxX - hb.minX, hb.maxY - hb.minY);
                drawNode->drawRect(rect, color, 1.0f, color);
            }
        }

        // 2. Dibujar Trayectoria Predictiva
        if (Mod::get()->getSettingValue<bool>("show-prediction")) {
            float pX = m_player1->getPositionX(), pY = m_player1->getPositionY();
            float vX = 8.4f * m_player1->m_speedMultiplier;
            float vY = AfterEngine::JUMP_VELOCITY;
            bool isSafe = AfterEngine::isJumpSafeFrom(pX, pY, m_player1);
            
            auto colPrimary =         auto* bg = cocos2d::CCLayerColor::create(cocos2d::ccc4(colSurface.r, colSurface.g, colSurface.b, 255), 300.f, 210.f);
        bg->setPosition({10.f, 10.f});
        m_mainLayer->addChild(bg, -1);

        auto* title = CCLabelBMFont::create("ULTIMATE ENGINE", "bigFont.fnt");
        title->setColor(colPrimary);
        title->setScale(0.6f);
        title->setPosition({160.f, 195.f});
        m_mainLayer->addChild(title);

        auto* menu = CCMenu::create();
        menu->setPosition({0, 0});
        m_mainLayer->addChild(menu);

        // Boton para alternar el modo de asistencia
        auto* toggleSpr = ButtonSprite::create("Cycle Assist Mode", "bigFont.fnt", "GJ_button_01.png", 0.6f);
        auto* toggleBtn = CCMenuItemSpriteExtra::create(toggleSpr, this, menu_selector(AfterPanel::onCycleMode));
        toggleBtn->setPosition({160.f, 140.f});
        menu->addChild(toggleBtn);

        // Boton para ver el HUD
        auto* hudSpr = ButtonSprite::create("Toggle Hitboxes", "bigFont.fnt", "GJ_button_04.png", 0.6f);
        auto* hudBtn = CCMenuItemSpriteExtra::create(hudSpr, this, menu_selector(AfterPanel::onToggleHitboxes));
        hudBtn->setPosition({160.f, 85.f});
        menu->addChild(hudBtn);

        // Boton para limpiar memoria
        auto* clrSpr = ButtonSprite::create("Clear Level Memory", "bigFont.fnt", "GJ_button_06.png", 0.6f);
        auto* clrBtn = CCMenuItemSpriteExtra::create(clrSpr, this, menu_selector(AfterPanel::onClearMemory));
        clrBtn->setPosition({160.f, 30.f});
        menu->addChild(clrBtn);

        return true;
    }

    void onCycleMode(CCObject*) {
        std::string mode = Mod::get()->getSettingValue<std::string>("assist-mode");
        if (mode == "Desactivado") Mod::get()->setSettingValue<std::string>("assist-mode", "Suave");
        else if (mode == "Suave") Mod::get()->setSettingValue<std::string>("assist-mode", "Balanceado");
        else if (mode == "Balanceado") Mod::get()->setSettingValue<std::string>("assist-mode", "Extremo");
        else Mod::get()->setSettingValue<std::string>("assist-mode", "Desactivado");
        
        FLAlertLayer::create("Modo Cambiado", ("Ahora: " + Mod::get()->getSettingValue<std::string>("assist-mode")).c_str(), "OK")->show();
    }

    void onToggleHitboxes(CCObject*) {
        bool hb = Mod::get()->getSettingValue<bool>("show-hitboxes");
        Mod::get()->setSettingValue<bool>("show-hitboxes", !hb);
    }

    void onClearMemory(CCObject*) {
        AfterEngine::g_levelMemory.clear();
        FLAlertLayer::create("Memoria", "Memoria de nivel borrada.", "OK")->show();
    }

public:
    static AfterPanel* create() {
        auto* ret = new AfterPanel();
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret; return nullptr;
    }
};

class AfterHUD : public cocos2d::CCLayer {
public:
    static AfterHUD* create() {
        auto* ret = new AfterHUD();
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret; return nullptr;
    }
    bool init() override {
        if (!CCLayer::init()) return false;
        auto colPrimary = Mod::get()->getSettingValue<cocos2d::ccColor3B>("ui-primary-color");
        auto c4 = cocos2d::ccc4f(colPrimary.r / 255.f, colPrimary.g / 255.f, colPrimary.b / 255.f, 1.f);
        
        auto* circle = cocos2d::CCDrawNode::create();
        circle->drawCircle({0, 0}, 22.f, cocos2d::ccc4f(0.1f, 0.1f, 0.1f, 0.9f), 2.f, c4, 32);
        circle->setID("am-circle");
        this->addChild(circle);
        
        auto* label = cocos2d::CCLabelBMFont::create("am", "bigFont.fnt");
        label->setScale(0.7f); label->setColor(colPrimary); this->addChild(label);
        
        this->setTouchEnabled(true); this->setTouchMode(cocos2d::kCCTouchesOneByOne);
        this->setTouchPriority(-500); this->setPosition({50, cocos2d::CCDirector::get()->getWinSize().height - 100});
        return true;
    }
    bool ccTouchBegan(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override {
        return this->convertTouchToNodeSpace(touch).getLengthSq() < 400.f;
    }
    void ccTouchMoved(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override {
        this->setPosition(this->getPosition() + touch->getDelta());
    }
    void ccTouchEnded(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override {
        auto delta = touch->getDelta();
        if (std::abs(delta.x) < 5 && std::abs(delta.y) < 5) AfterPanel::create()->show();
    }
    void registerWithTouchDispatcher() override {
        cocos2d::CCDirector::get()->getTouchDispatcher()->addTargetedDelegate(this, -500, true);
    }
};

// =====================================================================================
// 3. INTERCEPCIÓN DE INPUTS (CBF, CAF, Buffer)
// =====================================================================================

class $modify(PlayerObjectHook, PlayerObject) {
    void pushButton(PlayerButton button) {
        if (button != PlayerButton::Jump) { PlayerObject::pushButton(button); return; }
        
        auto* pl = PlayLayer::get();
        if (!pl) return;
        int p = (this == pl->m_player1) ? 0 : 1;
        std::string mode = Mod::get()->getSettingValue<std::string>("assist-mode");

        AfterEngine::g_holding[p] = true;

        if (this->m_isOnGround) {
            if (mode == "Desactivado") {
                AfterEngine::attemptLateClickRewind(this, p);
            } else {
                AfterEngine::g_bufferedJump[p] = true;
                AfterEngine::g_jumpStart[p] = AfterEngine::g_currentTick;
                AfterEngine::g_usedRewind[p] = false;
            }
        } else {
            PlayerObject::pushButton(button);
        }
    }

    void releaseButton(PlayerButton button) {
        if (button != PlayerButton::Jump) { PlayerObject::releaseButton(button); return; }
        auto* pl = PlayLayer::get();
        if (!pl) return;
        int p = (this == pl->m_player1) ? 0 : 1;
        
        AfterEngine::g_holding[p] = false;
        if (AfterEngine::g_bufferedJump[p]) { 
            AfterEngine::g_bufferedJump[p] = false; 
            return; 
        }
        PlayerObject::releaseButton(button);
    }
};

class $modify(InputAssistBase, GJBaseGameLayer) {
    void update(float dt) {
        AfterEngine::g_currentTick++;
        auto* pl = PlayLayer::get();
        if (!pl || !pl->m_player1) { GJBaseGameLayer::update(dt); return; }

        AfterEngine::g_playerHistory.push_back({
            pl->m_player1->getPositionX(), pl->m_player1->getPositionY(),
            pl->m_player1->m_yVelocity, pl->m_player1->m_isOnGround,
            AfterEngine::g_currentTick, pl->m_player1->m_isShip,
            pl->m_player1->m_isDart, pl->m_player1->m_isBird, pl->m_player1->m_isBall
        });
        if (AfterEngine::g_playerHistory.size() > AfterEngine::MAX_HISTORY) {
            AfterEngine::g_playerHistory.pop_front();
        }

        if (Mod::get()->getSettingValue<std::string>("assist-mode") != "Desactivado") {
            AfterEngine::updateCache(pl, pl->m_player1);

            for (int i = 0; i < 2; i++) {
                PlayerObject* player = (i == 0) ? pl->m_player1 : pl->m_player2;
                if (!player) continue;

                if (AfterEngine::g_bufferedJump[i]) {
                    int maxTicks = AfterEngine::getMaxTicks(player);
                    uint64_t elapsed = AfterEngine::g_currentTick - AfterEngine::g_jumpStart[i];

                    if (player->m_isOnGround) {
                        bool safe = AfterEngine::isJumpSafeFrom(player->getPositionX(), player->getPositionY(), player);
                        
                        if (safe || elapsed >= maxTicks) {
                            player->pushButton(PlayerButton::Jump);
                            AfterEngine::g_bufferedJump[i] = false;
                            
                            if (Mod::get()->getSettingValue<bool>("level-memory")) {
                                AfterEngine::g_levelMemory[(int)(player->getPositionX() / 30)] = elapsed;
                            }

                            if (!AfterEngine::g_holding[i]) player->releaseButton(PlayerButton::Jump);
                        }
                    } else {
                        AfterEngine::g_bufferedJump[i] = false;
                    }
                }
            }
        }
        GJBaseGameLayer::update(dt);
    }
};

// =====================================================================================
// 4. HOOKS DE UI Y DIBUJO
// =====================================================================================

class $modify(MenuLayerHook, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        if (Mod::get()->getSettingValue<bool>("show-hud")) this->addChild(AfterHUD::create(), 9999);
        return true;
    }
};

class $modify(PauseLayerHookUI, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto* menu = getChildByID("right-button-menu");
        if (!menu) return;
        auto* btn = CCMenuItemSpriteExtra::create(CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png"), this, menu_selector(PauseLayerHookUI::abrirPanel));
        menu->addChild(btn);
    }
    void abrirPanel(CCObject*) { AfterPanel::create()->show(); }
};

class $modify(InputAssistPlay, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        AfterEngine::g_playerHistory.clear();
        AfterEngine::g_replayIndex = 0;
        if (Mod::get()->getSettingValue<bool>("show-hud")) this->addChild(AfterHUD::create(), 9999);
        return true;
    }

    void draw() {
        PlayLayer::draw();
        if (!m_player1) return;
        
        auto* drawNode = typeinfo_cast<cocos2d::CCDrawNode*>(this->getChildByID("aero-draw-node"));
        if (!drawNode) {
            drawNode = cocos2d::CCDrawNode::create();
            drawNode->setID("aero-draw-node");
            this->addChild(drawNode, 100);
        }
        drawNode->clear();

        if (Mod::get()->getSettingValue<bool>("show-hitboxes")) {
            for (size_t i = 0; i < AfterEngine::g_hitboxCount; i++) {
                auto& hb = AfterEngine::g_cachedHitboxes[i];
                if (hb.isPortal || hb.isOrb) continue;
                
                cocos2d::ccColor4F color = hb.isHazard ? cocos2d::ccc4f(1, 0, 0, 0.5f) : cocos2d::ccc4f(0, 1, 0, 0.5f);
                cocos2d::CCRect rect(hb.minX, hb.minY, hb.maxX - hb.minX, hb.maxY - hb.minY);
                drawNode->drawRect(rect, color, 1.0f, color);
            }
        }

        if (Mod::get()->getSettingValue<bool>("show-prediction")) {
            float pX = m_player1->getPositionX(), pY = m_player1->getPositionY();
            float vX = 8.4f * m_player1->m_speedMultiplier;
            float vY = AfterEngine::JUMP_VELOCITY;
            bool isSafe = AfterEngine::isJumpSafeFrom(pX, pY, m_player1);
            
            auto colPrimary = Mod::get()->getSettingValue<cocos2d::ccColor3B>("ui-primary-color");
            cocos2d::ccColor4F color = isSafe ? 
                cocos2d::ccc4f(colPrimary.r/255.f, colPrimary.g/255.f, colPrimary.b/255.f, 0.6f) : 
                cocos2d::ccc4f(1, 0, 0, 0.6f);

            for (int i = 1; i <= 25; ++i) {
                float nextX = pX + (vX * i);
                float nextY = pY + (vY * i) - (0.5f * AfterEngine::GRAVITY * i * i);
                drawNode->drawDot({nextX, nextY}, 2.5f, color);
                if (nextY <= pY && i > 3) break;
            }
        }
    }
};
