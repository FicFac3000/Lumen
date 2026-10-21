#include "lumen/Show.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <sstream>

namespace lumen {

namespace {

// ---------------------------------------------------------------------------
// Thin RAII over the sqlite3 C API. No wrapper dependency: the build should
// work from a clean checkout with nothing fetched.
// ---------------------------------------------------------------------------
class Db {
public:
    ~Db() { close(); }

    bool open(const std::string& path, bool create) {
        const int flags = create ? (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)
                                 : SQLITE_OPEN_READONLY;
        if (sqlite3_open_v2(path.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
            err_ = db_ ? sqlite3_errmsg(db_) : "cannot open";
            close();
            return false;
        }
        // Crash safety is the whole reason for choosing SQLite, so make the
        // guarantees explicit rather than relying on defaults.
        exec("PRAGMA journal_mode=WAL");
        exec("PRAGMA synchronous=FULL");
        exec("PRAGMA foreign_keys=ON");
        return true;
    }

    void close() {
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
    }

    bool exec(const std::string& sql) {
        char* msg = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &msg) != SQLITE_OK) {
            err_ = msg ? msg : "exec failed";
            if (msg) sqlite3_free(msg);
            return false;
        }
        return true;
    }

    sqlite3* raw() { return db_; }
    const std::string& error() const { return err_; }
    void setError(std::string e) { err_ = std::move(e); }

private:
    sqlite3* db_ = nullptr;
    std::string err_;
};

class Stmt {
public:
    Stmt(Db& db, const std::string& sql) : db_(&db) {
        if (sqlite3_prepare_v2(db.raw(), sql.c_str(), -1, &st_, nullptr) != SQLITE_OK) {
            db.setError(sqlite3_errmsg(db.raw()));
            st_ = nullptr;
        }
    }
    ~Stmt() {
        if (st_) sqlite3_finalize(st_);
    }

    bool valid() const { return st_ != nullptr; }

    Stmt& bind(int i, int64_t v) { sqlite3_bind_int64(st_, i, v); return *this; }
    Stmt& bind(int i, int v) { sqlite3_bind_int(st_, i, v); return *this; }
    Stmt& bind(int i, double v) { sqlite3_bind_double(st_, i, v); return *this; }
    Stmt& bind(int i, const std::string& v) {
        sqlite3_bind_text(st_, i, v.c_str(), -1, SQLITE_TRANSIENT);
        return *this;
    }
    Stmt& bindUid(int i, Uid u) { return bind(i, static_cast<int64_t>(u.v)); }

    bool step() { return sqlite3_step(st_) == SQLITE_ROW; }
    bool run() {
        const int rc = sqlite3_step(st_);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            db_->setError(sqlite3_errmsg(db_->raw()));
            return false;
        }
        return true;
    }
    void reset() {
        sqlite3_reset(st_);
        sqlite3_clear_bindings(st_);
    }

    int64_t i64(int c) const { return sqlite3_column_int64(st_, c); }
    int i32(int c) const { return sqlite3_column_int(st_, c); }
    double dbl(int c) const { return sqlite3_column_double(st_, c); }
    Uid uid(int c) const { return Uid{static_cast<uint64_t>(sqlite3_column_int64(st_, c))}; }
    std::string text(int c) const {
        const unsigned char* p = sqlite3_column_text(st_, c);
        return p ? reinterpret_cast<const char*>(p) : std::string{};
    }
    bool isNull(int c) const { return sqlite3_column_type(st_, c) == SQLITE_NULL; }

private:
    Db* db_;
    sqlite3_stmt* st_ = nullptr;
};

const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS meta (
  key TEXT PRIMARY KEY, value TEXT);

CREATE TABLE IF NOT EXISTS fixture_type (
  uid INTEGER PRIMARY KEY, name TEXT NOT NULL, cell_count INTEGER NOT NULL);

CREATE TABLE IF NOT EXISTS type_channel (
  type_uid INTEGER NOT NULL, attr INTEGER NOT NULL, coarse INTEGER NOT NULL,
  fine INTEGER NOT NULL, cell INTEGER NOT NULL, default_value INTEGER NOT NULL,
  invert INTEGER NOT NULL,
  FOREIGN KEY(type_uid) REFERENCES fixture_type(uid) ON DELETE CASCADE);

CREATE TABLE IF NOT EXISTS type_set (
  type_uid INTEGER NOT NULL, attr INTEGER NOT NULL, cell INTEGER NOT NULL,
  name TEXT NOT NULL, dmx_from INTEGER NOT NULL, dmx_to INTEGER NOT NULL,
  phys_from REAL NOT NULL, phys_to REAL NOT NULL, anchor INTEGER NOT NULL,
  FOREIGN KEY(type_uid) REFERENCES fixture_type(uid) ON DELETE CASCADE);

CREATE TABLE IF NOT EXISTS fixture (
  uid INTEGER PRIMARY KEY, fid INTEGER NOT NULL, name TEXT NOT NULL,
  type_uid INTEGER NOT NULL, universe INTEGER NOT NULL, address INTEGER NOT NULL,
  ord INTEGER NOT NULL,
  x REAL NOT NULL, y REAL NOT NULL, z REAL NOT NULL,
  rx REAL NOT NULL, ry REAL NOT NULL, rz REAL NOT NULL, positioned INTEGER NOT NULL,
  FOREIGN KEY(type_uid) REFERENCES fixture_type(uid));

CREATE TABLE IF NOT EXISTS fixture_tag (
  fixture_uid INTEGER NOT NULL, tag TEXT NOT NULL,
  FOREIGN KEY(fixture_uid) REFERENCES fixture(uid) ON DELETE CASCADE);

CREATE TABLE IF NOT EXISTS sequence (
  uid INTEGER PRIMARY KEY, sid INTEGER NOT NULL, name TEXT NOT NULL,
  chase INTEGER NOT NULL, chase_rate REAL NOT NULL, chase_dir INTEGER NOT NULL,
  chase_xfade REAL NOT NULL, track_default INTEGER NOT NULL);

CREATE TABLE IF NOT EXISTS cue (
  uid INTEGER PRIMARY KEY, seq_uid INTEGER NOT NULL, idx INTEGER NOT NULL,
  number REAL NOT NULL, name TEXT NOT NULL,
  fade_in REAL NOT NULL, fade_out REAL NOT NULL,
  delay_in REAL NOT NULL, delay_out REAL NOT NULL, curve INTEGER NOT NULL,
  shield INTEGER NOT NULL, trigger INTEGER NOT NULL, trigger_time REAL NOT NULL,
  track_mask INTEGER NOT NULL,
  FOREIGN KEY(seq_uid) REFERENCES sequence(uid) ON DELETE CASCADE);

CREATE TABLE IF NOT EXISTS cue_part (
  cue_uid INTEGER NOT NULL, part INTEGER NOT NULL, name TEXT NOT NULL,
  has_timing INTEGER NOT NULL,
  fade_in REAL NOT NULL, fade_out REAL NOT NULL,
  delay_in REAL NOT NULL, delay_out REAL NOT NULL, curve INTEGER NOT NULL,
  FOREIGN KEY(cue_uid) REFERENCES cue(uid) ON DELETE CASCADE);

CREATE TABLE IF NOT EXISTS cue_value (
  cue_uid INTEGER NOT NULL, part INTEGER NOT NULL,
  fixture INTEGER NOT NULL, cell INTEGER NOT NULL, attr INTEGER NOT NULL,
  kind INTEGER NOT NULL, value REAL NOT NULL, active INTEGER NOT NULL,
  preset_uid INTEGER NOT NULL, preset_attr INTEGER NOT NULL,
  FOREIGN KEY(cue_uid) REFERENCES cue(uid) ON DELETE CASCADE);

CREATE TABLE IF NOT EXISTS phaser (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  cue_uid INTEGER, attr INTEGER NOT NULL, role INTEGER NOT NULL,
  gen_kind INTEGER NOT NULL, wave INTEGER NOT NULL, duty REAL NOT NULL,
  size REAL NOT NULL, centre REAL NOT NULL, relative INTEGER NOT NULL,
  speed REAL NOT NULL, source INTEGER NOT NULL,
  blocks INTEGER NOT NULL, wings INTEGER NOT NULL, groups REAL NOT NULL,
  offset REAL NOT NULL,
  direction INTEGER NOT NULL, pivot INTEGER NOT NULL, overshoot REAL NOT NULL,
  gap_on INTEGER NOT NULL, gap_len REAL NOT NULL, gap_frac INTEGER NOT NULL,
  gap_nth INTEGER NOT NULL, gap_hold INTEGER NOT NULL, gap_place INTEGER NOT NULL,
  gap_resume INTEGER NOT NULL, gap_scope INTEGER NOT NULL, gap_trans REAL NOT NULL,
  var_phase REAL NOT NULL, var_speed REAL NOT NULL, var_size REAL NOT NULL,
  var_seed INTEGER NOT NULL,
  stack INTEGER NOT NULL, clock INTEGER NOT NULL, fade REAL NOT NULL,
  FOREIGN KEY(cue_uid) REFERENCES cue(uid) ON DELETE CASCADE);

CREATE TABLE IF NOT EXISTS phaser_target (
  phaser_id INTEGER NOT NULL, fixture INTEGER NOT NULL, cell INTEGER NOT NULL,
  ord INTEGER NOT NULL,
  FOREIGN KEY(phaser_id) REFERENCES phaser(id) ON DELETE CASCADE);

CREATE TABLE IF NOT EXISTS phaser_step (
  phaser_id INTEGER NOT NULL, ord INTEGER NOT NULL,
  value REAL NOT NULL, width REAL NOT NULL, attack REAL NOT NULL, decay REAL NOT NULL,
  attack_curve REAL NOT NULL, decay_curve REAL NOT NULL,
  FOREIGN KEY(phaser_id) REFERENCES phaser(id) ON DELETE CASCADE);

CREATE TABLE IF NOT EXISTS phaser_curve (
  phaser_id INTEGER NOT NULL, ord INTEGER NOT NULL,
  x REAL NOT NULL, y REAL NOT NULL,
  in_dx REAL NOT NULL, in_dy REAL NOT NULL, out_dx REAL NOT NULL, out_dy REAL NOT NULL,
  FOREIGN KEY(phaser_id) REFERENCES phaser(id) ON DELETE CASCADE);

CREATE TABLE IF NOT EXISTS phaser_gap_seq (
  phaser_id INTEGER NOT NULL, ord INTEGER NOT NULL, length REAL NOT NULL,
  FOREIGN KEY(phaser_id) REFERENCES phaser(id) ON DELETE CASCADE);

CREATE TABLE IF NOT EXISTS executor (
  uid INTEGER PRIMARY KEY, eid INTEGER NOT NULL, seq_id INTEGER NOT NULL,
  label TEXT NOT NULL, priority INTEGER NOT NULL, merge INTEGER NOT NULL,
  swap_group INTEGER NOT NULL, master REAL NOT NULL,
  release_mode INTEGER NOT NULL, release_time REAL NOT NULL);

CREATE TABLE IF NOT EXISTS executor_merge (
  exec_uid INTEGER NOT NULL, grp INTEGER NOT NULL, rule INTEGER NOT NULL,
  FOREIGN KEY(exec_uid) REFERENCES executor(uid) ON DELETE CASCADE);

CREATE TABLE IF NOT EXISTS executor_release (
  exec_uid INTEGER NOT NULL, grp INTEGER NOT NULL, mode INTEGER NOT NULL,
  FOREIGN KEY(exec_uid) REFERENCES executor(uid) ON DELETE CASCADE);

CREATE INDEX IF NOT EXISTS ix_cue_seq ON cue(seq_uid, idx);
CREATE INDEX IF NOT EXISTS ix_val_cue ON cue_value(cue_uid);
CREATE INDEX IF NOT EXISTS ix_chan_type ON type_channel(type_uid);
)SQL";

// The five per-group tracking modes pack into one integer, two bits each.
int64_t packTrack(const std::array<TrackMode, kGroupCount>& t) {
    int64_t v = 0;
    for (size_t i = 0; i < kGroupCount; ++i)
        v |= static_cast<int64_t>(t[i]) << (2 * i);
    return v;
}

void unpackTrack(int64_t v, std::array<TrackMode, kGroupCount>& out) {
    for (size_t i = 0; i < kGroupCount; ++i) {
        const auto m = static_cast<int>((v >> (2 * i)) & 0x3);
        out[i] = m <= 2 ? static_cast<TrackMode>(m) : TrackMode::Track;
    }
}

bool writeMeta(Db& db, const std::string& key, const std::string& value) {
    Stmt s(db, "INSERT OR REPLACE INTO meta(key,value) VALUES(?,?)");
    if (!s.valid()) return false;
    s.bind(1, key).bind(2, value);
    return s.run();
}

std::string readMeta(Db& db, const std::string& key) {
    Stmt s(db, "SELECT value FROM meta WHERE key=?");
    if (!s.valid()) return {};
    s.bind(1, key);
    return s.step() ? s.text(0) : std::string{};
}

} // namespace

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------
SaveReport ShowFile::save(const Engine& eng, const std::string& path,
                          const std::string& showName) {
    SaveReport rep;

    // Write to a sibling temp file, then replace atomically. A transaction
    // alone protects the file being written; this also protects the previous
    // good file if the process dies between open and commit.
    const std::string tmp = path + ".tmp";
    std::error_code ec;
    std::filesystem::remove(tmp, ec);

    Db db;
    if (!db.open(tmp, true)) {
        rep.error = "open: " + db.error();
        return rep;
    }
    if (!db.exec(kSchema)) {
        rep.error = "schema: " + db.error();
        return rep;
    }
    if (!db.exec("BEGIN IMMEDIATE")) {
        rep.error = "begin: " + db.error();
        return rep;
    }

    auto fail = [&](const std::string& what) {
        db.exec("ROLLBACK");
        db.close();
        std::filesystem::remove(tmp, ec);
        rep.ok = false;
        rep.error = what + ": " + db.error();
        return rep;
    };

    writeMeta(db, "schema_version", std::to_string(kShowSchemaVersion));
    writeMeta(db, "show_name", showName);
    writeMeta(db, "grand_master", std::to_string(eng.grandMaster));
    writeMeta(db, "blackout", eng.blackout ? "1" : "0");
    writeMeta(db, "unused_mode",
              eng.unusedMode == UnusedMode::HoldLast ? "hold" : "default");

    // --- fixture types ----------------------------------------------------
    {
        Stmt ty(db, "INSERT INTO fixture_type(uid,name,cell_count) VALUES(?,?,?)");
        Stmt ch(db, "INSERT INTO type_channel(type_uid,attr,coarse,fine,cell,"
                    "default_value,invert) VALUES(?,?,?,?,?,?,?)");
        Stmt st(db, "INSERT INTO type_set(type_uid,attr,cell,name,dmx_from,dmx_to,"
                    "phys_from,phys_to,anchor) VALUES(?,?,?,?,?,?,?,?,?)");
        if (!ty.valid() || !ch.valid() || !st.valid()) return fail("prepare types");

        for (int i = 0; i < eng.patch.typeCount(); ++i) {
            const FixtureType& t = eng.patch.type(i);
            ty.reset();
            ty.bindUid(1, t.uid).bind(2, t.name).bind(3, static_cast<int>(t.cellCount));
            if (!ty.run()) return fail("write type");
            ++rep.types;

            for (const auto& c : t.channels) {
                ch.reset();
                ch.bindUid(1, t.uid)
                  .bind(2, static_cast<int>(c.attr))
                  .bind(3, c.coarse)
                  .bind(4, c.fine)
                  .bind(5, static_cast<int>(c.cell))
                  .bind(6, static_cast<int>(c.defaultValue))
                  .bind(7, c.invert ? 1 : 0);
                if (!ch.run()) return fail("write channel");
            }
            for (const auto& s : t.sets) {
                st.reset();
                st.bindUid(1, t.uid)
                  .bind(2, static_cast<int>(s.attr))
                  .bind(3, static_cast<int>(s.cell))
                  .bind(4, s.name)
                  .bind(5, static_cast<int>(s.dmxFrom))
                  .bind(6, static_cast<int>(s.dmxTo))
                  .bind(7, static_cast<double>(s.physFrom))
                  .bind(8, static_cast<double>(s.physTo))
                  .bind(9, s.anchor ? 1 : 0);
                if (!st.run()) return fail("write set");
            }
        }
    }

    // --- fixtures ---------------------------------------------------------
    {
        Stmt fx(db, "INSERT INTO fixture(uid,fid,name,type_uid,universe,address,ord,"
                    "x,y,z,rx,ry,rz,positioned) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        Stmt tg(db, "INSERT INTO fixture_tag(fixture_uid,tag) VALUES(?,?)");
        if (!fx.valid() || !tg.valid()) return fail("prepare fixtures");

        for (const Fixture& f : eng.patch.fixtures()) {
            const FixtureType& t = eng.patch.type(f.typeIndex);
            fx.reset();
            fx.bindUid(1, f.uid)
              .bind(2, static_cast<int64_t>(f.id))
              .bind(3, f.name)
              .bindUid(4, t.uid)
              .bind(5, f.universe)
              .bind(6, f.address)
              .bind(7, f.order)
              .bind(8, static_cast<double>(f.x))
              .bind(9, static_cast<double>(f.y))
              .bind(10, static_cast<double>(f.z))
              .bind(11, static_cast<double>(f.rotX))
              .bind(12, static_cast<double>(f.rotY))
              .bind(13, static_cast<double>(f.rotZ))
              .bind(14, f.positioned ? 1 : 0);
            if (!fx.run()) return fail("write fixture");
            ++rep.fixtures;

            for (const auto& tag : f.tags) {
                tg.reset();
                tg.bindUid(1, f.uid).bind(2, tag);
                if (!tg.run()) return fail("write tag");
            }
        }
    }

    // --- sequences, cues, values, phasers ---------------------------------
    {
        Stmt sq(db, "INSERT INTO sequence(uid,sid,name,chase,chase_rate,chase_dir,"
                    "chase_xfade,track_default) VALUES(?,?,?,?,?,?,?,?)");
        Stmt cu(db, "INSERT INTO cue(uid,seq_uid,idx,number,name,fade_in,fade_out,"
                    "delay_in,delay_out,curve,shield,trigger,trigger_time,track_mask) "
                    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        Stmt cp(db, "INSERT INTO cue_part(cue_uid,part,name,has_timing,fade_in,"
                    "fade_out,delay_in,delay_out,curve) VALUES(?,?,?,?,?,?,?,?,?)");
        Stmt cv(db, "INSERT INTO cue_value(cue_uid,part,fixture,cell,attr,kind,value,"
                    "active,preset_uid,preset_attr) VALUES(?,?,?,?,?,?,?,?,?,?)");
        Stmt ph(db, "INSERT INTO phaser(cue_uid,attr,role,gen_kind,wave,duty,size,centre,"
                    "relative,speed,source,blocks,wings,groups,offset,direction,pivot,"
                    "overshoot,gap_on,gap_len,gap_frac,gap_nth,gap_hold,gap_place,"
                    "gap_resume,gap_scope,gap_trans,var_phase,var_speed,var_size,"
                    "var_seed,stack,clock,fade) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,"
                    "?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        Stmt pt(db, "INSERT INTO phaser_target(phaser_id,fixture,cell,ord) VALUES(?,?,?,?)");
        Stmt ps(db, "INSERT INTO phaser_step(phaser_id,ord,value,width,attack,decay,"
                    "attack_curve,decay_curve) VALUES(?,?,?,?,?,?,?,?)");
        Stmt pc(db, "INSERT INTO phaser_curve(phaser_id,ord,x,y,in_dx,in_dy,out_dx,out_dy) "
                    "VALUES(?,?,?,?,?,?,?,?)");
        Stmt pg(db, "INSERT INTO phaser_gap_seq(phaser_id,ord,length) VALUES(?,?,?)");
        if (!sq.valid() || !cu.valid() || !cp.valid() || !cv.valid() || !ph.valid() ||
            !pt.valid() || !ps.valid() || !pc.valid() || !pg.valid())
            return fail("prepare sequences");

        for (const Sequence& s : eng.sequences) {
            sq.reset();
            sq.bindUid(1, s.uid)
              .bind(2, s.id)
              .bind(3, s.name)
              .bind(4, s.chase ? 1 : 0)
              .bind(5, static_cast<double>(s.chaseRateHz))
              .bind(6, static_cast<int>(s.chaseDirection))
              .bind(7, static_cast<double>(s.chaseCrossfade))
              .bind(8, packTrack(s.trackDefault));
            if (!sq.run()) return fail("write sequence");
            ++rep.sequences;

            for (size_t ci = 0; ci < s.cues.size(); ++ci) {
                const Cue& c = s.cues[ci];
                cu.reset();
                cu.bindUid(1, c.uid)
                  .bindUid(2, s.uid)
                  .bind(3, static_cast<int>(ci))
                  .bind(4, static_cast<double>(c.number))
                  .bind(5, c.name)
                  .bind(6, c.timing.fadeIn)
                  .bind(7, c.timing.fadeOut)
                  .bind(8, c.timing.delayIn)
                  .bind(9, c.timing.delayOut)
                  .bind(10, static_cast<int>(c.timing.curve))
                  .bind(11, c.shield ? 1 : 0)
                  .bind(12, static_cast<int>(c.trigger))
                  .bind(13, c.triggerTime)
                  .bind(14, packTrack(c.track));
                if (!cu.run()) return fail("write cue");
                ++rep.cues;

                for (size_t pi = 0; pi < c.parts.size(); ++pi) {
                    const CuePart& part = c.parts[pi];
                    const Timing pt2 = part.timing ? *part.timing : Timing{};
                    cp.reset();
                    cp.bindUid(1, c.uid)
                      .bind(2, static_cast<int>(pi))
                      .bind(3, part.name)
                      .bind(4, part.timing ? 1 : 0)
                      .bind(5, pt2.fadeIn)
                      .bind(6, pt2.fadeOut)
                      .bind(7, pt2.delayIn)
                      .bind(8, pt2.delayOut)
                      .bind(9, static_cast<int>(pt2.curve));
                    if (!cp.run()) return fail("write part");

                    for (const auto& [key, val] : part.values.values()) {
                        cv.reset();
                        cv.bindUid(1, c.uid)
                          .bind(2, static_cast<int>(pi))
                          .bind(3, static_cast<int64_t>(keyFixture(key)))
                          .bind(4, static_cast<int>(keyCell(key)))
                          .bind(5, static_cast<int>(keyAttr(key)))
                          .bind(6, static_cast<int>(val.kind))
                          .bind(7, static_cast<double>(val.v))
                          .bind(8, val.active ? 1 : 0)
                          .bindUid(9, val.preset)
                          .bind(10, static_cast<int>(val.presetAttr));
                        if (!cv.run()) return fail("write value");
                        ++rep.values;
                    }
                }

                for (const Phaser& p : c.phasers) {
                    ph.reset();
                    // Bind positions are written out explicitly: incrementing
                    // a counter inside one chained expression is unsequenced,
                    // so the argument order would be undefined.
                    ph.bindUid(1, c.uid)
                      .bind(2, static_cast<int>(p.attr))
                      .bind(3, static_cast<int>(p.role))
                      .bind(4, static_cast<int>(p.gen.kind))
                      .bind(5, static_cast<int>(p.gen.wave))
                      .bind(6, static_cast<double>(p.gen.duty))
                      .bind(7, static_cast<double>(p.size))
                      .bind(8, static_cast<double>(p.centre))
                      .bind(9, p.relative ? 1 : 0)
                      .bind(10, static_cast<double>(p.speedHz))
                      .bind(11, static_cast<int>(p.source))
                      .bind(12, p.mods.blocks)
                      .bind(13, p.mods.wings)
                      .bind(14, static_cast<double>(p.mods.groups))
                      .bind(15, static_cast<double>(p.mods.offset))
                      .bind(16, static_cast<int>(p.direction))
                      .bind(17, static_cast<int>(p.pivot))
                      .bind(18, static_cast<double>(p.overshoot))
                      .bind(19, p.gap.enabled ? 1 : 0)
                      .bind(20, static_cast<double>(p.gap.length))
                      .bind(21, p.gap.lengthIsFraction ? 1 : 0)
                      .bind(22, p.gap.everyNth)
                      .bind(23, static_cast<int>(p.gap.hold))
                      .bind(24, static_cast<int>(p.gap.placement))
                      .bind(25, static_cast<int>(p.gap.resume))
                      .bind(26, static_cast<int>(p.gap.scope))
                      .bind(27, static_cast<double>(p.gap.transition))
                      .bind(28, static_cast<double>(p.variation.phase))
                      .bind(29, static_cast<double>(p.variation.speed))
                      .bind(30, static_cast<double>(p.variation.size))
                      .bind(31, static_cast<int64_t>(p.variation.seed))
                      .bind(32, static_cast<int>(p.stack))
                      .bind(33, static_cast<int>(p.clock))
                      .bind(34, static_cast<double>(p.fade));
                    if (!ph.run()) return fail("write phaser");
                    const int64_t pid = sqlite3_last_insert_rowid(db.raw());
                    ++rep.phasers;

                    for (size_t i = 0; i < p.targets.size(); ++i) {
                        pt.reset();
                        pt.bind(1, pid)
                          .bind(2, static_cast<int64_t>(p.targets[i].fixture))
                          .bind(3, static_cast<int>(p.targets[i].cell))
                          .bind(4, static_cast<int>(i));
                        if (!pt.run()) return fail("write phaser target");
                    }
                    for (size_t i = 0; i < p.gen.steps.size(); ++i) {
                        const Step& s2 = p.gen.steps[i];
                        ps.reset();
                        ps.bind(1, pid).bind(2, static_cast<int>(i))
                          .bind(3, static_cast<double>(s2.value))
                          .bind(4, static_cast<double>(s2.width))
                          .bind(5, static_cast<double>(s2.attack))
                          .bind(6, static_cast<double>(s2.decay))
                          .bind(7, static_cast<double>(s2.attackCurve))
                          .bind(8, static_cast<double>(s2.decayCurve));
                        if (!ps.run()) return fail("write step");
                    }
                    for (size_t i = 0; i < p.gen.curve.size(); ++i) {
                        const CurvePoint& q = p.gen.curve[i];
                        pc.reset();
                        pc.bind(1, pid).bind(2, static_cast<int>(i))
                          .bind(3, static_cast<double>(q.x))
                          .bind(4, static_cast<double>(q.y))
                          .bind(5, static_cast<double>(q.inDx))
                          .bind(6, static_cast<double>(q.inDy))
                          .bind(7, static_cast<double>(q.outDx))
                          .bind(8, static_cast<double>(q.outDy));
                        if (!pc.run()) return fail("write curve point");
                    }
                    for (size_t i = 0; i < p.gap.sequence.size(); ++i) {
                        pg.reset();
                        pg.bind(1, pid).bind(2, static_cast<int>(i))
                          .bind(3, static_cast<double>(p.gap.sequence[i]));
                        if (!pg.run()) return fail("write gap sequence");
                    }
                }
            }
        }
    }

    // --- executors --------------------------------------------------------
    {
        Stmt ex(db, "INSERT INTO executor(uid,eid,seq_id,label,priority,merge,swap_group,"
                    "master,release_mode,release_time) VALUES(?,?,?,?,?,?,?,?,?,?)");
        Stmt em(db, "INSERT INTO executor_merge(exec_uid,grp,rule) VALUES(?,?,?)");
        Stmt er(db, "INSERT INTO executor_release(exec_uid,grp,mode) VALUES(?,?,?)");
        if (!ex.valid() || !em.valid() || !er.valid()) return fail("prepare executors");

        for (const Executor& e : eng.executors) {
            ex.reset();
            ex.bindUid(1, e.uid)
              .bind(2, e.id)
              .bind(3, e.sequenceId)
              .bind(4, e.label)
              .bind(5, e.priority)
              .bind(6, static_cast<int>(e.merge))
              .bind(7, e.swapGroup)
              .bind(8, static_cast<double>(e.master))
              .bind(9, static_cast<int>(e.release))
              .bind(10, e.releaseTime);
            if (!ex.run()) return fail("write executor");
            ++rep.executors;

            for (size_t g = 0; g < kGroupCount; ++g) {
                if (e.mergeByGroup[g]) {
                    em.reset();
                    em.bindUid(1, e.uid).bind(2, static_cast<int>(g))
                      .bind(3, static_cast<int>(*e.mergeByGroup[g]));
                    if (!em.run()) return fail("write exec merge");
                }
                if (e.releaseByGroup[g]) {
                    er.reset();
                    er.bindUid(1, e.uid).bind(2, static_cast<int>(g))
                      .bind(3, static_cast<int>(*e.releaseByGroup[g]));
                    if (!er.run()) return fail("write exec release");
                }
            }
        }
    }

    if (!db.exec("COMMIT")) return fail("commit");
    db.close();

    // Replace the previous file only once a complete, committed one exists.
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        rep.error = "rename: " + ec.message();
        return rep;
    }

    rep.ok = true;
    return rep;
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------
LoadReport ShowFile::inspect(const std::string& path) {
    LoadReport rep;
    Db db;
    if (!db.open(path, false)) {
        rep.error = "open: " + db.error();
        return rep;
    }
    const std::string v = readMeta(db, "schema_version");
    rep.schemaVersion = v.empty() ? 0 : std::stoi(v);
    rep.showName = readMeta(db, "show_name");

    Stmt c(db, "SELECT (SELECT COUNT(*) FROM fixture),(SELECT COUNT(*) FROM sequence),"
               "(SELECT COUNT(*) FROM cue),(SELECT COUNT(*) FROM executor)");
    if (c.valid() && c.step()) {
        rep.fixtures = c.i32(0);
        rep.sequences = c.i32(1);
        rep.cues = c.i32(2);
        rep.executors = c.i32(3);
    }
    rep.ok = true;
    return rep;
}

LoadReport ShowFile::load(Engine& eng, const std::string& path) {
    LoadReport rep;
    Db db;
    if (!db.open(path, false)) {
        rep.error = "open: " + db.error();
        return rep;
    }

    const std::string sv = readMeta(db, "schema_version");
    rep.schemaVersion = sv.empty() ? 0 : std::stoi(sv);
    if (rep.schemaVersion > kShowSchemaVersion) {
        // Refuse a newer file rather than silently misreading it.
        rep.error = "show was written by a newer version (schema " + sv + ", this build " +
                    std::to_string(kShowSchemaVersion) + ")";
        return rep;
    }
    rep.showName = readMeta(db, "show_name");

    // Build into locals first, so a failure part-way leaves the engine as it
    // was rather than half-replaced.
    Patch patch(eng.uids);
    std::unordered_map<uint64_t, int> typeIndexByUid;

    {
        Stmt ty(db, "SELECT uid,name,cell_count FROM fixture_type ORDER BY rowid");
        if (!ty.valid()) { rep.error = "read types"; return rep; }
        while (ty.step()) {
            FixtureType t;
            t.uid = ty.uid(0);
            t.name = ty.text(1);
            t.cellCount = static_cast<CellId>(ty.i32(2));
            eng.uids.observe(t.uid);

            Stmt ch(db, "SELECT attr,coarse,fine,cell,default_value,invert FROM "
                        "type_channel WHERE type_uid=? ORDER BY rowid");
            ch.bindUid(1, t.uid);
            while (ch.step()) {
                ChannelDef c;
                c.attr = static_cast<Attr>(ch.i32(0));
                c.coarse = ch.i32(1);
                c.fine = ch.i32(2);
                c.cell = static_cast<CellId>(ch.i32(3));
                c.defaultValue = static_cast<uint8_t>(ch.i32(4));
                c.invert = ch.i32(5) != 0;
                t.channels.push_back(c);
            }
            Stmt st(db, "SELECT attr,cell,name,dmx_from,dmx_to,phys_from,phys_to,anchor "
                        "FROM type_set WHERE type_uid=? ORDER BY rowid");
            st.bindUid(1, t.uid);
            while (st.step()) {
                ChannelSet s;
                s.attr = static_cast<Attr>(st.i32(0));
                s.cell = static_cast<CellId>(st.i32(1));
                s.name = st.text(2);
                s.dmxFrom = static_cast<uint8_t>(st.i32(3));
                s.dmxTo = static_cast<uint8_t>(st.i32(4));
                s.physFrom = static_cast<float>(st.dbl(5));
                s.physTo = static_cast<float>(st.dbl(6));
                s.anchor = st.i32(7) != 0;
                t.sets.push_back(s);
            }
            const Uid uid = t.uid;
            typeIndexByUid[uid.v] = patch.addType(std::move(t));
        }
    }

    {
        Stmt fx(db, "SELECT uid,fid,name,type_uid,universe,address,ord,x,y,z,rx,ry,rz,"
                    "positioned FROM fixture ORDER BY ord");
        if (!fx.valid()) { rep.error = "read fixtures"; return rep; }
        while (fx.step()) {
            const Uid uid = fx.uid(0);
            const auto fid = static_cast<FixtureId>(fx.i64(1));
            const Uid tuid = fx.uid(3);
            auto it = typeIndexByUid.find(tuid.v);
            if (it == typeIndexByUid.end()) {
                rep.warnings.push_back("fixture " + std::to_string(fid) +
                                       " references an unknown type; skipped");
                continue;
            }
            Patch::PatchRequest req;
            req.id = fid;
            req.name = fx.text(2);
            req.typeIndex = it->second;
            req.universe = fx.i32(4);
            req.address = fx.i32(5);
            req.uid = uid;
            std::string err;
            if (!patch.add(req, &err)) {
                rep.warnings.push_back("fixture " + std::to_string(fid) + ": " + err);
                continue;
            }
            // Position: the UID came in through the request, so the by-UID
            // index is already correct.
            auto* f = const_cast<Fixture*>(patch.find(fid));
            f->x = static_cast<float>(fx.dbl(7));
            f->y = static_cast<float>(fx.dbl(8));
            f->z = static_cast<float>(fx.dbl(9));
            f->rotX = static_cast<float>(fx.dbl(10));
            f->rotY = static_cast<float>(fx.dbl(11));
            f->rotZ = static_cast<float>(fx.dbl(12));
            f->positioned = fx.i32(13) != 0;
            eng.uids.observe(uid);

            Stmt tg(db, "SELECT tag FROM fixture_tag WHERE fixture_uid=?");
            tg.bindUid(1, uid);
            while (tg.step()) f->tags.push_back(tg.text(0));
            ++rep.fixtures;
        }
    }

    std::deque<Sequence> seqs;
    {
        Stmt sq(db, "SELECT uid,sid,name,chase,chase_rate,chase_dir,chase_xfade,"
                    "track_default FROM sequence ORDER BY sid");
        if (!sq.valid()) { rep.error = "read sequences"; return rep; }
        while (sq.step()) {
            Sequence s;
            s.uid = sq.uid(0);
            s.id = sq.i32(1);
            s.name = sq.text(2);
            s.chase = sq.i32(3) != 0;
            s.chaseRateHz = static_cast<float>(sq.dbl(4));
            s.chaseDirection = static_cast<Direction>(sq.i32(5));
            s.chaseCrossfade = static_cast<float>(sq.dbl(6));
            unpackTrack(sq.i64(7), s.trackDefault);
            eng.uids.observe(s.uid);

            Stmt cu(db, "SELECT uid,idx,number,name,fade_in,fade_out,delay_in,delay_out,"
                        "curve,shield,trigger,trigger_time,track_mask FROM cue "
                        "WHERE seq_uid=? ORDER BY idx");
            cu.bindUid(1, s.uid);
            while (cu.step()) {
                Cue c;
                c.uid = cu.uid(0);
                c.number = static_cast<float>(cu.dbl(2));
                c.name = cu.text(3);
                c.timing.fadeIn = cu.dbl(4);
                c.timing.fadeOut = cu.dbl(5);
                c.timing.delayIn = cu.dbl(6);
                c.timing.delayOut = cu.dbl(7);
                c.timing.curve = static_cast<FadeCurve>(cu.i32(8));
                c.shield = cu.i32(9) != 0;
                c.trigger = static_cast<Cue::Trigger>(cu.i32(10));
                c.triggerTime = cu.dbl(11);
                unpackTrack(cu.i64(12), c.track);
                eng.uids.observe(c.uid);

                Stmt cp(db, "SELECT part,name,has_timing,fade_in,fade_out,delay_in,"
                            "delay_out,curve FROM cue_part WHERE cue_uid=? ORDER BY part");
                cp.bindUid(1, c.uid);
                while (cp.step()) {
                    const auto pi = static_cast<size_t>(cp.i32(0));
                    while (c.parts.size() <= pi) c.parts.push_back(CuePart{});
                    c.parts[pi].name = cp.text(1);
                    if (cp.i32(2) != 0) {
                        Timing t2;
                        t2.fadeIn = cp.dbl(3);
                        t2.fadeOut = cp.dbl(4);
                        t2.delayIn = cp.dbl(5);
                        t2.delayOut = cp.dbl(6);
                        t2.curve = static_cast<FadeCurve>(cp.i32(7));
                        c.parts[pi].timing = t2;
                    }
                }

                Stmt cv(db, "SELECT part,fixture,cell,attr,kind,value,active,preset_uid,"
                            "preset_attr FROM cue_value WHERE cue_uid=?");
                cv.bindUid(1, c.uid);
                while (cv.step()) {
                    const auto part = static_cast<size_t>(cv.i32(0));
                    while (c.parts.size() <= part) c.parts.push_back(CuePart{});
                    Value v;
                    v.kind = static_cast<Value::Kind>(cv.i32(4));
                    v.v = static_cast<float>(cv.dbl(5));
                    v.active = cv.i32(6) != 0;
                    v.preset = cv.uid(7);
                    v.presetAttr = static_cast<Attr>(cv.i32(8));
                    c.parts[part].values.set(static_cast<FixtureId>(cv.i64(1)),
                                             static_cast<CellId>(cv.i32(2)),
                                             static_cast<Attr>(cv.i32(3)), v);
                }

                Stmt ph(db, "SELECT id,attr,role,gen_kind,wave,duty,size,centre,relative,"
                            "speed,source,blocks,wings,groups,offset,direction,pivot,"
                            "overshoot,gap_on,gap_len,gap_frac,gap_nth,gap_hold,gap_place,"
                            "gap_resume,gap_scope,gap_trans,var_phase,var_speed,var_size,"
                            "var_seed,stack,clock,fade FROM phaser WHERE cue_uid=? "
                            "ORDER BY id");
                ph.bindUid(1, c.uid);
                while (ph.step()) {
                    const int64_t pid = ph.i64(0);
                    Phaser p;
                    p.attr = static_cast<Attr>(ph.i32(1));
                    p.role = static_cast<Role>(ph.i32(2));
                    p.gen.kind = static_cast<GenKind>(ph.i32(3));
                    p.gen.wave = static_cast<Wave>(ph.i32(4));
                    p.gen.duty = static_cast<float>(ph.dbl(5));
                    p.size = static_cast<float>(ph.dbl(6));
                    p.centre = static_cast<float>(ph.dbl(7));
                    p.relative = ph.i32(8) != 0;
                    p.speedHz = static_cast<float>(ph.dbl(9));
                    p.source = static_cast<SpreadSource>(ph.i32(10));
                    p.mods.blocks = ph.i32(11);
                    p.mods.wings = ph.i32(12);
                    p.mods.groups = static_cast<float>(ph.dbl(13));
                    p.mods.offset = static_cast<float>(ph.dbl(14));
                    p.direction = static_cast<Direction>(ph.i32(15));
                    p.pivot = static_cast<BouncePivot>(ph.i32(16));
                    p.overshoot = static_cast<float>(ph.dbl(17));
                    p.gap.enabled = ph.i32(18) != 0;
                    p.gap.length = static_cast<float>(ph.dbl(19));
                    p.gap.lengthIsFraction = ph.i32(20) != 0;
                    p.gap.everyNth = ph.i32(21);
                    p.gap.hold = static_cast<GapHold>(ph.i32(22));
                    p.gap.placement = static_cast<GapPlacement>(ph.i32(23));
                    p.gap.resume = static_cast<GapResume>(ph.i32(24));
                    p.gap.scope = static_cast<GapScope>(ph.i32(25));
                    p.gap.transition = static_cast<float>(ph.dbl(26));
                    p.variation.phase = static_cast<float>(ph.dbl(27));
                    p.variation.speed = static_cast<float>(ph.dbl(28));
                    p.variation.size = static_cast<float>(ph.dbl(29));
                    p.variation.seed = static_cast<uint64_t>(ph.i64(30));
                    p.stack = static_cast<StackMode>(ph.i32(31));
                    p.clock = static_cast<ClockSource>(ph.i32(32));
                    p.fade = static_cast<float>(ph.dbl(33));

                    Stmt pt(db, "SELECT fixture,cell FROM phaser_target WHERE phaser_id=? "
                                "ORDER BY ord");
                    pt.bind(1, pid);
                    while (pt.step())
                        p.targets.push_back({static_cast<FixtureId>(pt.i64(0)),
                                             static_cast<CellId>(pt.i32(1))});

                    Stmt ps(db, "SELECT value,width,attack,decay,attack_curve,decay_curve "
                                "FROM phaser_step WHERE phaser_id=? ORDER BY ord");
                    ps.bind(1, pid);
                    while (ps.step()) {
                        Step s2;
                        s2.value = static_cast<float>(ps.dbl(0));
                        s2.width = static_cast<float>(ps.dbl(1));
                        s2.attack = static_cast<float>(ps.dbl(2));
                        s2.decay = static_cast<float>(ps.dbl(3));
                        s2.attackCurve = static_cast<float>(ps.dbl(4));
                        s2.decayCurve = static_cast<float>(ps.dbl(5));
                        p.gen.steps.push_back(s2);
                    }

                    Stmt pc(db, "SELECT x,y,in_dx,in_dy,out_dx,out_dy FROM phaser_curve "
                                "WHERE phaser_id=? ORDER BY ord");
                    pc.bind(1, pid);
                    while (pc.step()) {
                        CurvePoint q;
                        q.x = static_cast<float>(pc.dbl(0));
                        q.y = static_cast<float>(pc.dbl(1));
                        q.inDx = static_cast<float>(pc.dbl(2));
                        q.inDy = static_cast<float>(pc.dbl(3));
                        q.outDx = static_cast<float>(pc.dbl(4));
                        q.outDy = static_cast<float>(pc.dbl(5));
                        p.gen.curve.push_back(q);
                    }

                    Stmt pg(db, "SELECT length FROM phaser_gap_seq WHERE phaser_id=? "
                                "ORDER BY ord");
                    pg.bind(1, pid);
                    while (pg.step()) p.gap.sequence.push_back(static_cast<float>(pg.dbl(0)));

                    c.phasers.push_back(std::move(p));
                }

                s.cues.push_back(std::move(c));
                ++rep.cues;
            }
            seqs.push_back(std::move(s));
            ++rep.sequences;
        }
    }

    std::deque<Executor> execs;
    {
        Stmt ex(db, "SELECT uid,eid,seq_id,label,priority,merge,swap_group,master,"
                    "release_mode,release_time FROM executor ORDER BY eid");
        if (!ex.valid()) { rep.error = "read executors"; return rep; }
        while (ex.step()) {
            Executor e;
            e.uid = ex.uid(0);
            e.id = ex.i32(1);
            e.sequenceId = ex.i32(2);
            e.label = ex.text(3);
            e.priority = ex.i32(4);
            e.merge = static_cast<MergeRule>(ex.i32(5));
            e.swapGroup = ex.i32(6);
            e.master = static_cast<float>(ex.dbl(7));
            e.release = static_cast<ReleaseMode>(ex.i32(8));
            e.releaseTime = ex.dbl(9);
            eng.uids.observe(e.uid);

            Stmt em(db, "SELECT grp,rule FROM executor_merge WHERE exec_uid=?");
            em.bindUid(1, e.uid);
            while (em.step()) {
                const auto g = static_cast<size_t>(em.i32(0));
                if (g < kGroupCount) e.mergeByGroup[g] = static_cast<MergeRule>(em.i32(1));
            }
            Stmt er(db, "SELECT grp,mode FROM executor_release WHERE exec_uid=?");
            er.bindUid(1, e.uid);
            while (er.step()) {
                const auto g = static_cast<size_t>(er.i32(0));
                if (g < kGroupCount) e.releaseByGroup[g] = static_cast<ReleaseMode>(er.i32(1));
            }
            execs.push_back(std::move(e));
            ++rep.executors;
        }
    }

    // Everything read without error: now swap it in.
    eng.patch = std::move(patch);
    eng.sequences = std::move(seqs);
    eng.executors = std::move(execs);
    eng.programmer.clearAll();
    eng.syncUniverses();

    const std::string gm = readMeta(db, "grand_master");
    if (!gm.empty()) eng.grandMaster = std::stof(gm);
    eng.blackout = readMeta(db, "blackout") == "1";
    eng.unusedMode = readMeta(db, "unused_mode") == "default" ? UnusedMode::ToDefault
                                                              : UnusedMode::HoldLast;
    rep.ok = true;
    return rep;
}

// ---------------------------------------------------------------------------
// Autosave
// ---------------------------------------------------------------------------
std::string Autosave::pathFor(int index) const {
    return dir_ + "/" + base_ + ".auto" + std::to_string(index) + ".lumen";
}

SaveReport Autosave::write(const Engine& eng, const std::string& reason) {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    rotate();
    SaveReport r = ShowFile::save(eng, pathFor(0), reason);
    ++counter_;
    return r;
}

void Autosave::rotate() {
    // Shift the rolling set down, discarding the oldest. Versions rather than
    // one overwritten file, so a bad save cannot take the show with it.
    std::error_code ec;
    std::filesystem::remove(pathFor(keep_ - 1), ec);
    for (int i = keep_ - 2; i >= 0; --i) {
        if (std::filesystem::exists(pathFor(i), ec))
            std::filesystem::rename(pathFor(i), pathFor(i + 1), ec);
    }
}

std::vector<std::string> Autosave::versions() const {
    std::vector<std::string> out;
    std::error_code ec;
    for (int i = 0; i < keep_; ++i)
        if (std::filesystem::exists(pathFor(i), ec)) out.push_back(pathFor(i));
    return out;
}

std::string Autosave::newest() const {
    const auto v = versions();
    return v.empty() ? std::string{} : v.front();
}

SaveReport Autosave::snapshot(const Engine& eng, const std::string& name) {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    // Kept outside the rolling set, so rotation cannot discard a snapshot the
    // operator took deliberately.
    return ShowFile::save(eng, dir_ + "/" + base_ + ".snap-" + name + ".lumen", name);
}

} // namespace lumen
