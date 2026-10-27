// Click Guide — a practice aid for Geometry Dash (Geode).
//
// How it works
//   * Every attempt, the mod records where you pressed and released jump.
//   * When you die, the clicks from the last few blocks before the death are
//     thrown away (that's probably where the mistake was). The rest of the
//     attempt is "known good".
//   * If the attempt got further than your saved guide already covered from
//     that starting point, the guide for that stretch is replaced with this
//     attempt's clicks. Beating the level saves the whole attempt.
//   * The guide is drawn inside the level: a dot where to press, a green trail
//     for how long to hold, and a shrinking ring around the NEXT click so you
//     can see the timing coming.
//
// Works in normal mode, practice mode and from start positions. Saved per
// level in the mod's save folder, so it is still there next time you play.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <vector>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

struct Click {
    bool p2 = false;               // true = player 2 (dual / 2-player levels)
    std::vector<CCPoint> path;     // path[0] = press, back() = release
};

struct Span { float a, b; };       // stretch of the level the guide covers

static float clickX(Click const& c) { return c.path.front().x; }

static bool setting(char const* key) {
    return Mod::get()->getSettingValue<bool>(key);
}
static float settingNum(char const* key) {
    return static_cast<float>(Mod::get()->getSettingValue<int64_t>(key));
}

// ---------------------------------------------------------------------------
// PlayLayer: recording, committing, saving
// ---------------------------------------------------------------------------

class GuideOverlay;

class $modify(CGPlayLayer, PlayLayer) {
    struct Fields {
        std::vector<Click> guide;      // saved good clicks, sorted by x
        std::vector<Span> covered;     // where the guide is known good
        std::vector<Click> attempt;    // clicks this attempt
        int held[2] = {-1, -1};        // index into attempt of a held click
        float attemptStartX = 0.f;
        bool attemptDone = false;
        bool dirty = false;
        std::string fileKey;
        GuideOverlay* overlay = nullptr;
    };

    // ---- file storage -----------------------------------------------------

    std::filesystem::path guidePath() {
        return Mod::get()->getSaveDir() / (m_fields->fileKey + ".txt");
    }

    void makeFileKey() {
        int id = m_level->m_levelID.value();
        if (id > 0) {
            m_fields->fileKey = "level_" + std::to_string(id);
            return;
        }
        // Local / editor levels have no ID, so use the name.
        std::string name = m_level->m_levelName;
        std::string safe;
        for (char ch : name) {
            safe += (std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_');
        }
        if (safe.empty()) safe = "unnamed";
        m_fields->fileKey = "local_" + safe;
    }

    void loadGuide() {
        auto& f = m_fields;
        f->guide.clear();
        f->covered.clear();
        std::ifstream in(guidePath());
        if (!in) return;
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream ss(line);
            std::string tag;
            ss >> tag;
            if (tag == "S") {
                Span s;
                if (ss >> s.a >> s.b) f->covered.push_back(s);
            } else if (tag == "C") {
                Click c;
                int p2 = 0, n = 0;
                if (!(ss >> p2 >> n) || n <= 0 || n > 100000) continue;
                c.p2 = p2 != 0;
                for (int i = 0; i < n; i++) {
                    float x, y;
                    if (!(ss >> x >> y)) break;
                    c.path.push_back({x, y});
                }
                if (!c.path.empty()) f->guide.push_back(std::move(c));
            }
        }
        sortGuide();
    }

    void saveGuide() {
        auto& f = m_fields;
        if (!f->dirty) return;
        std::ofstream out(guidePath(), std::ios::trunc);
        if (!out) {
            log::warn("Click Guide: couldn't write {}", guidePath().string());
            return;
        }
        out << "CG1\n";
        for (auto& s : f->covered) out << "S " << s.a << ' ' << s.b << '\n';
        for (auto& c : f->guide) {
            out << "C " << (c.p2 ? 1 : 0) << ' ' << c.path.size();
            for (auto& p : c.path) out << ' ' << p.x << ' ' << p.y;
            out << '\n';
        }
        f->dirty = false;
    }

    void sortGuide() {
        std::stable_sort(m_fields->guide.begin(), m_fields->guide.end(),
            [](Click const& a, Click const& b) { return clickX(a) < clickX(b); });
    }

    // ---- recording ----------------------------------------------------------

    bool isRecording() {
        if (!setting("record")) return false;
        if (setting("only-practice") && !m_isPracticeMode) return false;
        return true;
    }

    PlayerObject* playerFor(bool p2) {
        return p2 && m_player2 ? m_player2 : m_player1;
    }

    void onJump(bool down, bool p2) {
        auto& f = m_fields;
        if (f->attemptDone || !isRecording()) return;
        auto player = playerFor(p2);
        if (!player || player->m_isDead) return;
        int slot = p2 ? 1 : 0;
        CCPoint pos = player->getPosition();

        if (down) {
            Click c;
            c.p2 = p2;
            c.path.push_back(pos);
            f->attempt.push_back(std::move(c));
            f->held[slot] = static_cast<int>(f->attempt.size()) - 1;
        } else if (f->held[slot] >= 0) {
            auto& c = f->attempt[f->held[slot]];
            if (pos.getDistance(c.path.back()) > 0.5f) c.path.push_back(pos);
            f->held[slot] = -1;
        }
    }

    // Called every frame by the overlay: extend the trail of held clicks.
    void sampleHolds() {
        auto& f = m_fields;
        for (int slot = 0; slot < 2; slot++) {
            int idx = f->held[slot];
            if (idx < 0 || idx >= static_cast<int>(f->attempt.size())) continue;
            auto player = playerFor(slot == 1);
            if (!player || player->m_isDead) continue;
            auto& c = f->attempt[idx];
            CCPoint pos = player->getPosition();
            if (pos.getDistance(c.path.back()) >= 8.f) c.path.push_back(pos);
        }
    }

    void releaseHolds() {
        auto& f = m_fields;
        for (int slot = 0; slot < 2; slot++) {
            int idx = f->held[slot];
            if (idx >= 0 && idx < static_cast<int>(f->attempt.size())) {
                auto player = playerFor(slot == 1);
                if (player) {
                    auto& c = f->attempt[idx];
                    CCPoint pos = player->getPosition();
                    if (pos.getDistance(c.path.back()) > 0.5f) c.path.push_back(pos);
                }
            }
            f->held[slot] = -1;
        }
    }

    // ---- committing a good stretch to the guide ------------------------------

    // How far the saved guide already reaches, continuously, from x.
    float coveredFrom(float x) {
        float reach = x;
        bool grew = true;
        while (grew) {
            grew = false;
            for (auto& s : m_fields->covered) {
                if (s.a <= reach + 1.f && s.b > reach) {
                    reach = s.b;
                    grew = true;
                }
            }
        }
        return reach;
    }

    void addSpan(float a, float b) {
        auto& cov = m_fields->covered;
        cov.push_back({a, b});
        std::sort(cov.begin(), cov.end(),
            [](Span const& l, Span const& r) { return l.a < r.a; });
        std::vector<Span> merged;
        for (auto& s : cov) {
            if (!merged.empty() && s.a <= merged.back().b + 1.f) {
                merged.back().b = std::max(merged.back().b, s.b);
            } else {
                merged.push_back(s);
            }
        }
        cov = std::move(merged);
    }

    void commitAttempt(float safeX) {
        auto& f = m_fields;
        if (f->attemptDone) return;
        f->attemptDone = true;
        releaseHolds();
        if (!isRecording()) return;

        float start = f->attemptStartX;
        if (safeX <= start + 1.f) return;            // died almost instantly
        if (safeX <= coveredFrom(start)) return;      // guide already better

        // Replace guide clicks in [start, safeX] with this attempt's clicks.
        auto& g = f->guide;
        g.erase(std::remove_if(g.begin(), g.end(), [&](Click const& c) {
            float x = clickX(c);
            return x >= start && x <= safeX;
        }), g.end());
        for (auto& c : f->attempt) {
            float x = clickX(c);
            if (x >= start && x <= safeX) g.push_back(c);
        }
        sortGuide();
        addSpan(start, safeX);
        f->dirty = true;
        saveGuide();
    }

    void clearGuide() {
        auto& f = m_fields;
        f->guide.clear();
        f->covered.clear();
        f->dirty = false;
        std::error_code ec;
        std::filesystem::remove(guidePath(), ec);
    }

    // ---- hooks -------------------------------------------------------------

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects);

    void resetLevel() {
        PlayLayer::resetLevel();
        auto& f = m_fields;
        f->attempt.clear();
        f->held[0] = f->held[1] = -1;
        f->attemptDone = false;
        f->attemptStartX = m_player1 ? m_player1->getPositionX() : 0.f;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        // GD "kills" the player with a hidden spike at level start as an
        // anticheat check; that isn't a real death.
        if (object == m_anticheatSpike) {
            PlayLayer::destroyPlayer(player, object);
            return;
        }
        PlayLayer::destroyPlayer(player, object);
        if (m_player1 && m_player1->m_isDead && !m_fields->attemptDone) {
            float deathX = player ? player->getPositionX() : m_player1->getPositionX();
            commitAttempt(deathX - settingNum("death-margin"));
        }
    }

    void levelComplete() {
        commitAttempt(1e9f);
        PlayLayer::levelComplete();
    }

    void onQuit() {
        saveGuide();
        PlayLayer::onQuit();
    }
};

// ---------------------------------------------------------------------------
// Overlay: draws the guide inside the level (it scrolls with the level)
// ---------------------------------------------------------------------------

class GuideOverlay : public CCDrawNode {
public:
    static GuideOverlay* create() {
        auto ret = new GuideOverlay();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    bool init() override {
        if (!CCDrawNode::init()) return false;
        this->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
        this->scheduleUpdate();
        return true;
    }

    void update(float) override {
        this->clear();
        auto pl = static_cast<CGPlayLayer*>(PlayLayer::get());
        if (!pl || !pl->m_player1) return;
        pl->sampleHolds();
        if (!setting("enabled")) return;

        auto& f = pl->m_fields;
        float px = pl->m_player1->getPositionX();
        float behind = 120.f;
        float ahead = settingNum("lookahead");

        // Saved guide.
        auto& g = f->guide;
        auto it = std::lower_bound(g.begin(), g.end(), px - behind,
            [](Click const& c, float x) { return clickX(c) < x; });
        bool nextShown[2] = {false, false};
        for (; it != g.end(); ++it) {
            float x = clickX(*it);
            if (x > px + ahead) break;
            auto player = it->p2 && pl->m_player2 ? pl->m_player2 : pl->m_player1;
            float playerX = player->getPositionX();
            bool passed = x < playerX;
            bool isNext = false;
            int slot = it->p2 ? 1 : 0;
            if (!passed && !nextShown[slot]) {
                isNext = true;
                nextShown[slot] = true;
            }
            drawClick(*it, passed, isNext, x - playerX, ahead);
        }

        // This attempt's own clicks, faint, so you can compare.
        if (setting("show-attempt")) {
            for (auto& c : f->attempt) {
                float x = clickX(c);
                if (x < px - behind || x > px + 20.f) continue;
                ccColor4F col = {1.f, 1.f, 1.f, 0.45f};
                for (size_t i = 1; i < c.path.size(); i++) {
                    this->drawSegment(c.path[i - 1], c.path[i], 1.2f, col);
                }
                this->drawDot(c.path.front(), 3.f, col);
            }
        }
    }

private:
    void ring(CCPoint center, float radius, float width, ccColor4F col) {
        constexpr int N = 28;
        CCPoint verts[N];
        for (int i = 0; i < N; i++) {
            float a = static_cast<float>(i) / N * 6.2831853f;
                verts[i] = CCPoint(center.x + std::cos(a) * radius, center.y + std::sin(a) * radius);
        }
        this->drawPolygon(verts, N, {0.f, 0.f, 0.f, 0.f}, width, col);
    }

    void drawClick(Click const& c, bool passed, bool isNext, float dist, float ahead) {
        float alpha = passed ? 0.25f : 0.95f;
        ccColor4F press = c.p2
            ? ccColor4F{1.f, 0.6f, 0.15f, alpha}   // orange for player 2
            : ccColor4F{0.2f, 0.9f, 1.f, alpha};   // cyan for player 1
        ccColor4F hold = {0.35f, 1.f, 0.45f, alpha * 0.85f};

        // Hold trail.
        if (c.path.size() > 1) {
            for (size_t i = 1; i < c.path.size(); i++) {
                this->drawSegment(c.path[i - 1], c.path[i], 2.6f, hold);
            }
            // Release point: hollow ring.
            this->ring(c.path.back(), 4.f, 1.f, hold);
        }

        CCPoint p = c.path.front();
        // Short vertical timing line through the press point.
        this->drawSegment({p.x, p.y - 22.f}, {p.x, p.y + 22.f}, 0.8f,
            {press.r, press.g, press.b, alpha * 0.6f});
        // Press point.
        this->drawDot(p, isNext ? 6.5f : 5.f, press);
        this->ring(p, isNext ? 9.f : 7.f, 1.f, {1.f, 1.f, 1.f, alpha});

        // Approach ring on the next click: shrinks to the dot as you reach it.
        if (isNext && setting("approach-ring") && dist >= 0.f) {
            float t = std::clamp(dist / std::max(ahead * 0.6f, 1.f), 0.f, 1.f);
            float radius = 9.f + t * 55.f;
            this->ring(p, radius, 1.6f, {1.f, 1.f, 1.f, 0.9f - t * 0.6f});
        }
    }
};

bool CGPlayLayer::init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
    if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
    makeFileKey();
    loadGuide();
    auto overlay = GuideOverlay::create();
    if (overlay && m_objectLayer) {
        m_objectLayer->addChild(overlay, 9999);
        m_fields->overlay = overlay;
    }
    m_fields->attemptStartX = m_player1 ? m_player1->getPositionX() : 0.f;
    return true;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

class $modify(CGBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (button != 1) return;   // 1 = jump; 2/3 are platformer left/right
        auto pl = PlayLayer::get();
        if (!pl || static_cast<GJBaseGameLayer*>(pl) != this) return;
        static_cast<CGPlayLayer*>(pl)->onJump(down, !isPlayer1);
    }
};

// ---------------------------------------------------------------------------
// Pause menu: "Clear Guide" button
// ---------------------------------------------------------------------------

class $modify(CGPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto spr = ButtonSprite::create("Clear Guide");
        spr->setScale(0.55f);
        auto btn = CCMenuItemSpriteExtra::create(
            spr, this, menu_selector(CGPauseLayer::onClearGuide));
        auto menu = CCMenu::create();
        menu->addChild(btn);
        auto win = CCDirector::get()->getWinSize();
        menu->setPosition({58.f, win.height - 22.f});
        menu->setID("click-guide-menu"_spr);
        this->addChild(menu, 10);
    }

    void onClearGuide(CCObject*) {
        createQuickPopup(
            "Clear Guide",
            "Delete all saved click markers for <cy>this level</c>?",
            "Cancel", "Clear",
            [](FLAlertLayer*, bool clear) {
                if (!clear) return;
                if (auto pl = PlayLayer::get()) {
                    static_cast<CGPlayLayer*>(pl)->clearGuide();
                }
            });
    }
};
