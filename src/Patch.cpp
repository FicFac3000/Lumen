#include "lumen/Patch.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace lumen {

namespace {
std::string lower(const std::string& s) {
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return o;
}
} // namespace

// ---------------------------------------------------------------------------
// FixtureType
// ---------------------------------------------------------------------------
int FixtureType::footprint() const {
    int last = 0;
    for (const auto& c : channels) {
        last = std::max(last, c.coarse + 1);
        if (c.fine >= 0) last = std::max(last, c.fine + 1);
    }
    return last;
}

const ChannelDef* FixtureType::channel(Attr a, CellId cell) const {
    for (const auto& c : channels)
        if (c.attr == a && c.cell == cell) return &c;
    return nullptr;
}

bool FixtureType::hasAttr(Attr a, CellId cell) const { return channel(a, cell) != nullptr; }

std::vector<ChannelSet> FixtureType::setsFor(Attr a, CellId cell) const {
    std::vector<ChannelSet> out;
    for (const auto& s : sets)
        if (s.attr == a && s.cell == cell) out.push_back(s);
    std::sort(out.begin(), out.end(),
              [](const ChannelSet& x, const ChannelSet& y) { return x.dmxFrom < y.dmxFrom; });
    return out;
}

ValueLayer FixtureType::highlightFor(FixtureId id) const {
    ValueLayer out;

    // Collect the cells this type addresses, including the parent.
    std::set<CellId> cells{0};
    for (const auto& c : channels) cells.insert(c.cell);

    bool anyIntensity = false;
    for (const auto& c : channels) {
        if (isIntensity(c.attr)) {
            out.set(id, c.cell, c.attr, Value::hard(1.f));
            anyIntensity = true;
        }
    }

    for (CellId cell : cells) {
        // No dimmer anywhere on this type: drive colour to white so the
        // fixture still becomes visibly identifiable.
        if (!anyIntensity) {
            for (Attr a : {Attr::Red, Attr::Green, Attr::Blue, Attr::White}) {
                if (hasAttr(a, cell)) out.set(id, cell, a, Value::hard(1.f));
            }
        } else {
            // Open the colour mix so highlight reads as white, not whatever
            // colour happened to be dialled in.
            for (Attr a : {Attr::Red, Attr::Green, Attr::Blue, Attr::White})
                if (hasAttr(a, cell)) out.set(id, cell, a, Value::hard(1.f));
            for (Attr a : {Attr::CyanSub, Attr::Magenta, Attr::Yellow})
                if (hasAttr(a, cell)) out.set(id, cell, a, Value::hard(0.f));
        }

        // Open the beam path: shutter open, no gobo, iris and zoom mid, no
        // frost or prism.
        if (hasAttr(Attr::Shutter, cell)) out.set(id, cell, Attr::Shutter, Value::hard(1.f));
        if (hasAttr(Attr::Iris, cell)) out.set(id, cell, Attr::Iris, Value::hard(1.f));
        if (hasAttr(Attr::Frost, cell)) out.set(id, cell, Attr::Frost, Value::hard(0.f));
        if (hasAttr(Attr::Gobo1, cell)) out.set(id, cell, Attr::Gobo1, Value::hard(0.f));
        if (hasAttr(Attr::Gobo2, cell)) out.set(id, cell, Attr::Gobo2, Value::hard(0.f));
        if (hasAttr(Attr::Prism1, cell)) out.set(id, cell, Attr::Prism1, Value::hard(0.f));
        if (hasAttr(Attr::Zoom, cell)) out.set(id, cell, Attr::Zoom, Value::hard(0.5f));
        if (hasAttr(Attr::Focus, cell)) out.set(id, cell, Attr::Focus, Value::hard(0.5f));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Patch — types
// ---------------------------------------------------------------------------
int Patch::addType(FixtureType t) {
    if (!t.uid.valid()) t.uid = uids_->next();
    const int existing = findType(t.name);
    if (existing >= 0) {
        t.uid = types_[static_cast<size_t>(existing)].uid;  // keep identity
        types_[static_cast<size_t>(existing)] = std::move(t);
        return existing;
    }
    types_.push_back(std::move(t));
    return static_cast<int>(types_.size()) - 1;
}

int Patch::findType(const std::string& name) const {
    const std::string k = lower(name);
    for (size_t i = 0; i < types_.size(); ++i)
        if (lower(types_[i].name) == k) return static_cast<int>(i);
    return -1;
}

const FixtureType& Patch::type(int index) const { return types_.at(static_cast<size_t>(index)); }

// ---------------------------------------------------------------------------
// Patch — fixtures
// ---------------------------------------------------------------------------
bool Patch::footprintFree(int universe, int address, int fp, FixtureId ignore,
                          const Fixture** clash) const {
    for (const auto& f : fixtures_) {
        if (f.id == ignore || f.universe != universe) continue;
        const int ofp = types_[static_cast<size_t>(f.typeIndex)].footprint();
        if (address <= f.address + ofp - 1 && f.address <= address + fp - 1) {
            if (clash) *clash = &f;
            return false;
        }
    }
    return true;
}

bool Patch::add(const PatchRequest& req, std::string* err) {
    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        return false;
    };
    if (req.id == 0) return fail("fixture id must be non-zero");
    if (req.typeIndex < 0 || req.typeIndex >= typeCount()) return fail("unknown fixture type");
    if (req.universe < 1) return fail("universe must be >= 1");
    if (byId_.count(req.id)) return fail("fixture " + std::to_string(req.id) + " already patched");

    const FixtureType& ft = types_[static_cast<size_t>(req.typeIndex)];
    const int fp = ft.footprint();

    // A footprint may not span universes (§17.2).
    if (req.address < 1 || req.address + fp - 1 > 512)
        return fail("address " + std::to_string(req.address) + " does not fit " +
                    std::to_string(fp) + " channels in one universe");

    const Fixture* clash = nullptr;
    if (!footprintFree(req.universe, req.address, fp, 0, &clash))
        return fail("address clash with fixture " + std::to_string(clash->id) + " at " +
                    std::to_string(clash->universe) + "." + std::to_string(clash->address));

    Fixture fx;
    fx.uid = uids_->next();
    fx.id = req.id;
    fx.name = req.name.empty() ? (ft.name + " " + std::to_string(req.id)) : req.name;
    fx.typeIndex = req.typeIndex;
    fx.universe = req.universe;
    fx.address = req.address;
    fx.order = static_cast<int>(fixtures_.size());

    byId_[fx.id] = fixtures_.size();
    byUid_[fx.uid.v] = fixtures_.size();
    fixtures_.push_back(std::move(fx));
    return true;
}

int Patch::nextFreeAddress(int universe, int footprint, int gap) const {
    if (footprint < 1 || footprint > 512) return 0;
    for (int addr = 1; addr + footprint - 1 <= 512; ++addr) {
        if (footprintFree(universe, addr, footprint + gap, 0, nullptr)) return addr;
    }
    return 0;
}

int Patch::addRange(FixtureId firstId, int count, int typeIndex, int universe,
                    int firstAddress, int gap, std::string* err) {
    if (typeIndex < 0 || typeIndex >= typeCount()) {
        if (err) *err = "unknown fixture type";
        return 0;
    }
    const int fp = types_[static_cast<size_t>(typeIndex)].footprint();
    int addr = firstAddress > 0 ? firstAddress : nextFreeAddress(universe, fp, gap);
    int made = 0;

    for (int i = 0; i < count; ++i) {
        if (addr <= 0 || addr + fp - 1 > 512) {
            if (err) *err = "universe " + std::to_string(universe) + " full after " +
                            std::to_string(made) + " fixtures";
            break;
        }
        PatchRequest req;
        req.id = firstId + static_cast<FixtureId>(i);
        req.typeIndex = typeIndex;
        req.universe = universe;
        req.address = addr;
        if (!add(req, err)) break;
        ++made;
        addr += fp + gap;
    }
    return made;
}

const Fixture* Patch::find(FixtureId id) const {
    auto it = byId_.find(id);
    return it == byId_.end() ? nullptr : &fixtures_[it->second];
}

const Fixture* Patch::findByUid(Uid uid) const {
    auto it = byUid_.find(uid.v);
    return it == byUid_.end() ? nullptr : &fixtures_[it->second];
}

bool Patch::readdress(FixtureId id, int universe, int address, std::string* err) {
    auto it = byId_.find(id);
    if (it == byId_.end()) {
        if (err) *err = "no fixture " + std::to_string(id);
        return false;
    }
    Fixture& f = fixtures_[it->second];
    const int fp = types_[static_cast<size_t>(f.typeIndex)].footprint();
    if (universe < 1 || address < 1 || address + fp - 1 > 512) {
        if (err) *err = "address does not fit in one universe";
        return false;
    }
    const Fixture* clash = nullptr;
    if (!footprintFree(universe, address, fp, id, &clash)) {
        if (err) *err = "address clash with fixture " + std::to_string(clash->id);
        return false;
    }
    f.universe = universe;
    f.address = address;
    return true;
}

bool Patch::renumber(FixtureId from, FixtureId to, std::string* err) {
    if (from == to) return true;
    auto it = byId_.find(from);
    if (it == byId_.end()) {
        if (err) *err = "no fixture " + std::to_string(from);
        return false;
    }
    if (byId_.count(to)) {
        if (err) *err = "fixture " + std::to_string(to) + " already exists";
        return false;
    }
    const size_t idx = it->second;
    byId_.erase(it);
    fixtures_[idx].id = to;
    byId_[to] = idx;
    return true;
}

bool Patch::remove(FixtureId id) {
    auto it = byId_.find(id);
    if (it == byId_.end()) return false;
    const size_t idx = it->second;
    const Uid uid = fixtures_[idx].uid;
    fixtures_.erase(fixtures_.begin() + static_cast<long>(idx));
    byId_.clear();
    byUid_.clear();
    for (size_t i = 0; i < fixtures_.size(); ++i) {
        byId_[fixtures_[i].id] = i;
        byUid_[fixtures_[i].uid.v] = i;
    }
    (void)uid;  // UIDs are never reused, so nothing to reclaim.
    return true;
}

void Patch::clear() {
    fixtures_.clear();
    byId_.clear();
    byUid_.clear();
}

std::vector<int> Patch::universeList() const {
    std::set<int> u;
    for (const auto& f : fixtures_) u.insert(f.universe);
    return std::vector<int>(u.begin(), u.end());
}

const FixtureType* Patch::typeOf(FixtureId id) const {
    const Fixture* f = find(id);
    if (!f || f->typeIndex < 0) return nullptr;
    return &types_[static_cast<size_t>(f->typeIndex)];
}

std::vector<CellId> Patch::cellsOf(FixtureId id) const {
    std::vector<CellId> out;
    const FixtureType* ft = typeOf(id);
    if (!ft) return out;
    if (ft->cellCount == 0) {
        out.push_back(0);
        return out;
    }
    for (CellId c = 1; c <= ft->cellCount; ++c) out.push_back(c);
    return out;
}

std::vector<Patch::Conflict> Patch::conflicts() const {
    std::vector<Conflict> out;
    for (size_t i = 0; i < fixtures_.size(); ++i) {
        const Fixture& a = fixtures_[i];
        const int afp = types_[static_cast<size_t>(a.typeIndex)].footprint();
        for (size_t j = i + 1; j < fixtures_.size(); ++j) {
            const Fixture& b = fixtures_[j];
            if (a.universe != b.universe) continue;
            const int bfp = types_[static_cast<size_t>(b.typeIndex)].footprint();
            if (a.address <= b.address + bfp - 1 && b.address <= a.address + afp - 1) {
                out.push_back({a.id, b.id, a.universe, a.address, "footprint overlap"});
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Starter library
// ---------------------------------------------------------------------------
void installStarterTypes(Patch& patch) {
    {
        FixtureType t;
        t.name = "Dimmer";
        t.channels = {{Attr::Dimmer, 0}};
        patch.addType(std::move(t));
    }
    {
        FixtureType t;
        t.name = "LED Par RGBW";
        t.channels = {
            {Attr::Dimmer, 0}, {Attr::Red, 1}, {Attr::Green, 2},
            {Attr::Blue, 3}, {Attr::White, 4}, {Attr::Shutter, 5},
        };
        t.sets = {
            {Attr::Shutter, 0, "Closed", 0, 7, 0.f, 0.f, true},
            {Attr::Shutter, 0, "Open", 8, 15, 1.f, 1.f, false},
            {Attr::Shutter, 0, "Strobe", 16, 255, 1.f, 25.f, false},
        };
        patch.addType(std::move(t));
    }
    {
        FixtureType t;
        t.name = "Moving Spot";
        t.channels = {
            {Attr::Pan, 0, 1},  {Attr::Tilt, 2, 3},
            {Attr::Dimmer, 4},  {Attr::Shutter, 5},
            {Attr::Red, 6},     {Attr::Green, 7},    {Attr::Blue, 8},
            {Attr::Gobo1, 9},   {Attr::Gobo1Spin, 10},
            {Attr::Zoom, 11},   {Attr::Focus, 12},
            {Attr::Prism1, 13}, {Attr::Prism1Spin, 14},
        };
        // Prism rotate: a bidirectional range with a semantic zero. This is
        // the case §7.3's auto-stepper must handle symmetrically.
        t.sets = {
            {Attr::Prism1Spin, 0, "CCW fast", 0, 15, -30.f, -5.f, false},
            {Attr::Prism1Spin, 0, "CCW slow", 16, 120, -5.f, -0.2f, false},
            {Attr::Prism1Spin, 0, "Stop", 121, 134, 0.f, 0.f, true},
            {Attr::Prism1Spin, 0, "CW slow", 135, 239, 0.2f, 5.f, false},
            {Attr::Prism1Spin, 0, "CW fast", 240, 255, 5.f, 30.f, false},
            {Attr::Gobo1, 0, "Open", 0, 9, 0.f, 0.f, true},
            {Attr::Gobo1, 0, "Gobo 1", 10, 19, 1.f, 1.f, false},
            {Attr::Gobo1, 0, "Gobo 2", 20, 29, 2.f, 2.f, false},
            {Attr::Gobo1, 0, "Gobo 3", 30, 39, 3.f, 3.f, false},
        };
        patch.addType(std::move(t));
    }
    {
        // 12-cell pixel bar: one patch entry, twelve addressable cells. This
        // is the multi-instance path (§1.3) the tree editor targets.
        FixtureType t;
        t.name = "Pixel Bar 12";
        t.cellCount = 12;
        t.channels.push_back({Attr::Dimmer, 0});  // master dimmer on the parent
        for (int i = 0; i < 12; ++i) {
            const int base = 1 + i * 3;
            const auto cell = static_cast<CellId>(i + 1);
            t.channels.push_back({Attr::Red, base + 0, -1, cell});
            t.channels.push_back({Attr::Green, base + 1, -1, cell});
            t.channels.push_back({Attr::Blue, base + 2, -1, cell});
        }
        patch.addType(std::move(t));
    }
}

} // namespace lumen
