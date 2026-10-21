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
    // An absent facet must stay NULL, so "no filter" and "filter on zero"
    // cannot collapse into the same stored row.
    Stmt& bindNull(int i) { sqlite3_bind_null(st_, i); return *this; }

    // A .gdtf package goes in as a blob. SQLITE_TRANSIENT, because the
    // vector may be gone before the statement runs.
    Stmt& bindBlob(int i, const std::vector<uint8_t>& v) {
        if (v.empty()) sqlite3_bind_null(st_, i);
        else sqlite3_bind_blob64(st_, i, v.data(), v.size(), SQLITE_TRANSIENT);
        return *this;
    }

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

    std::vector<uint8_t> blob(int c) const {
        const void* p = sqlite3_column_blob(st_, c);
        const int n = sqlite3_column_bytes(st_, c);
        if (!p || n <= 0) return {};
        const auto* b = static_cast<const uint8_t*>(p);
        return std::vector<uint8_t>(b, b + n);
    }

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

CREATE TABLE IF NOT EXISTS grp (
  uid INTEGER PRIMARY KEY, gid INTEGER NOT NULL, name TEXT NOT NULL,
  kind INTEGER NOT NULL, allow_overrides INTEGER NOT NULL, ord INTEGER NOT NULL,
  q_type TEXT, q_universe INTEGER, q_addr_from INTEGER, q_addr_to INTEGER,
  q_id_from INTEGER, q_id_to INTEGER,
  q_attr INTEGER, q_group INTEGER, q_kind INTEGER, q_sort TEXT NOT NULL);

CREATE TABLE IF NOT EXISTS grp_member (
  group_uid INTEGER NOT NULL, fixture INTEGER NOT NULL, cell INTEGER NOT NULL,
  ord INTEGER NOT NULL,
  FOREIGN KEY(group_uid) REFERENCES grp(uid) ON DELETE CASCADE);

CREATE TABLE IF NOT EXISTS grp_child (
  group_uid INTEGER NOT NULL, child_uid INTEGER NOT NULL, ord INTEGER NOT NULL,
  FOREIGN KEY(group_uid) REFERENCES grp(uid) ON DELETE CASCADE);

-- Overrides and query tags are sticky by UID in both directions (SPEC 20.8),
-- so they persist as their own rows rather than being folded into membership.
CREATE TABLE IF NOT EXISTS grp_override (
  group_uid INTEGER NOT NULL, fixture_uid INTEGER NOT NULL, included INTEGER NOT NULL,
  FOREIGN KEY(group_uid) REFERENCES grp(uid) ON DELETE CASCADE);

CREATE TABLE IF NOT EXISTS grp_tag (
  group_uid INTEGER NOT NULL, tag TEXT NOT NULL, is_query INTEGER NOT NULL,
  FOREIGN KEY(group_uid) REFERENCES grp(uid) ON DELETE CASCADE);

CREATE TABLE IF NOT EXISTS preset_pool (
  pid INTEGER PRIMARY KEY, name TEXT NOT NULL, grp INTEGER NOT NULL,
  ord INTEGER NOT NULL);

CREATE TABLE IF NOT EXISTS preset (
  uid INTEGER PRIMARY KEY, pool INTEGER NOT NULL, pid INTEGER NOT NULL,
  name TEXT NOT NULL, grp INTEGER NOT NULL, generated INTEGER NOT NULL,
  symbol_override TEXT NOT NULL, indicator_override TEXT NOT NULL,
  ord INTEGER NOT NULL);

CREATE TABLE IF NOT EXISTS preset_value (
  preset_uid INTEGER NOT NULL,
  fixture INTEGER NOT NULL, cell INTEGER NOT NULL, attr INTEGER NOT NULL,
  kind INTEGER NOT NULL, value REAL NOT NULL, active INTEGER NOT NULL,
  ref_uid INTEGER NOT NULL, ref_attr INTEGER NOT NULL,
  FOREIGN KEY(preset_uid) REFERENCES preset(uid) ON DELETE CASCADE);

CREATE TABLE IF NOT EXISTS preset_tag (
  preset_uid INTEGER NOT NULL, tag TEXT NOT NULL,
  FOREIGN KEY(preset_uid) REFERENCES preset(uid) ON DELETE CASCADE);

CREATE TABLE IF NOT EXISTS recipe (
  uid INTEGER PRIMARY KEY, rid INTEGER NOT NULL, name TEXT NOT NULL,
  speed REAL NOT NULL, size REAL NOT NULL, spread REAL NOT NULL,
  offset REAL NOT NULL, base_hz REAL NOT NULL, ord INTEGER NOT NULL);

CREATE TABLE IF NOT EXISTS recipe_tag (
  recipe_uid INTEGER NOT NULL, tag TEXT NOT NULL,
  FOREIGN KEY(recipe_uid) REFERENCES recipe(uid) ON DELETE CASCADE);

CREATE TABLE IF NOT EXISTS recipe_slot (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  recipe_uid INTEGER NOT NULL, ord INTEGER NOT NULL, label TEXT NOT NULL,
  attr INTEGER NOT NULL, role INTEGER NOT NULL,
  gen_kind INTEGER NOT NULL, wave INTEGER NOT NULL, duty REAL NOT NULL,
  relative INTEGER NOT NULL, size_mul REAL NOT NULL, speed_mul REAL NOT NULL,
  phase_offset REAL NOT NULL, centre REAL NOT NULL,
  blocks INTEGER NOT NULL, wings INTEGER NOT NULL, groups REAL NOT NULL,
  offset REAL NOT NULL, source INTEGER NOT NULL,
  direction INTEGER NOT NULL, pivot INTEGER NOT NULL,
  gap_on INTEGER NOT NULL, gap_len REAL NOT NULL, gap_frac INTEGER NOT NULL,
  gap_nth INTEGER NOT NULL, gap_hold INTEGER NOT NULL, gap_place INTEGER NOT NULL,
  gap_resume INTEGER NOT NULL, gap_scope INTEGER NOT NULL, gap_trans REAL NOT NULL,
  var_phase REAL NOT NULL, var_speed REAL NOT NULL, var_size REAL NOT NULL,
  var_seed INTEGER NOT NULL,
  layer INTEGER NOT NULL, stack INTEGER NOT NULL, opacity REAL NOT NULL,
  muted INTEGER NOT NULL, soloed INTEGER NOT NULL,
  FOREIGN KEY(recipe_uid) REFERENCES recipe(uid) ON DELETE CASCADE);

CREATE TABLE IF NOT EXISTS recipe_slot_step (
  slot_id INTEGER NOT NULL, ord INTEGER NOT NULL,
  value REAL NOT NULL, width REAL NOT NULL, attack REAL NOT NULL, decay REAL NOT NULL,
  attack_curve REAL NOT NULL, decay_curve REAL NOT NULL,
  FOREIGN KEY(slot_id) REFERENCES recipe_slot(id) ON DELETE CASCADE);

CREATE TABLE IF NOT EXISTS recipe_slot_curve (
  slot_id INTEGER NOT NULL, ord INTEGER NOT NULL,
  x REAL NOT NULL, y REAL NOT NULL,
  in_dx REAL NOT NULL, in_dy REAL NOT NULL, out_dx REAL NOT NULL, out_dy REAL NOT NULL,
  FOREIGN KEY(slot_id) REFERENCES recipe_slot(id) ON DELETE CASCADE);

CREATE TABLE IF NOT EXISTS recipe_slot_gap_seq (
  slot_id INTEGER NOT NULL, ord INTEGER NOT NULL, length REAL NOT NULL,
  FOREIGN KEY(slot_id) REFERENCES recipe_slot(id) ON DELETE CASCADE);

-- The fixture library. `package` is the original .gdtf, kept verbatim and
-- EMBEDDED rather than referenced: a profile is tens of kilobytes, and a show
-- that opens with its fixtures missing because a library folder moved is not a
-- show (SPEC 1.4, 17.7). The parsed definition is stored alongside it, because
-- the two are allowed to diverge — that is what `edited` means.
CREATE TABLE IF NOT EXISTS library (
  uid INTEGER PRIMARY KEY, lid INTEGER NOT NULL, name TEXT NOT NULL,
  source TEXT NOT NULL, edited INTEGER NOT NULL,
  manufacturer TEXT NOT NULL, fixture_name TEXT NOT NULL, short_name TEXT NOT NULL,
  type_id TEXT NOT NULL, description TEXT NOT NULL,
  package BLOB, ord INTEGER NOT NULL);

CREATE TABLE IF NOT EXISTS library_tag (
  library_uid INTEGER NOT NULL, tag TEXT NOT NULL,
  FOREIGN KEY(library_uid) REFERENCES library(uid) ON DELETE CASCADE);

-- The geometry tree, flattened by parent NAME. Names are unique within a
-- definition (validation rejects duplicates), and rows are written in
-- pre-order so a parent always exists before its children.
CREATE TABLE IF NOT EXISTS library_geometry (
  library_uid INTEGER NOT NULL, ord INTEGER NOT NULL, parent TEXT NOT NULL,
  name TEXT NOT NULL, kind INTEGER NOT NULL, referenced TEXT NOT NULL,
  dmx_offset INTEGER NOT NULL,
  FOREIGN KEY(library_uid) REFERENCES library(uid) ON DELETE CASCADE);

CREATE TABLE IF NOT EXISTS library_mode (
  library_uid INTEGER NOT NULL, ord INTEGER NOT NULL, name TEXT NOT NULL,
  root TEXT NOT NULL,
  FOREIGN KEY(library_uid) REFERENCES library(uid) ON DELETE CASCADE);

CREATE TABLE IF NOT EXISTS library_channel (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  library_uid INTEGER NOT NULL, mode_ord INTEGER NOT NULL, ord INTEGER NOT NULL,
  attr INTEGER NOT NULL, geometry TEXT NOT NULL,
  coarse INTEGER NOT NULL, fine INTEGER NOT NULL,
  default_value INTEGER NOT NULL, invert INTEGER NOT NULL,
  FOREIGN KEY(library_uid) REFERENCES library(uid) ON DELETE CASCADE);

CREATE TABLE IF NOT EXISTS library_channel_set (
  channel_id INTEGER NOT NULL, ord INTEGER NOT NULL, name TEXT NOT NULL,
  dmx_from INTEGER NOT NULL, dmx_to INTEGER NOT NULL,
  phys_from REAL NOT NULL, phys_to REAL NOT NULL, anchor INTEGER NOT NULL,
  FOREIGN KEY(channel_id) REFERENCES library_channel(id) ON DELETE CASCADE);

CREATE TABLE IF NOT EXISTS library_revision (
  library_uid INTEGER NOT NULL, ord INTEGER NOT NULL, text TEXT NOT NULL,
  date TEXT NOT NULL,
  FOREIGN KEY(library_uid) REFERENCES library(uid) ON DELETE CASCADE);

-- Binding sets (SPEC 12.6): stored in the show and swappable, so a
-- controller map travels with the show rather than with the machine.
CREATE TABLE IF NOT EXISTS binding_set (
  uid INTEGER PRIMARY KEY, bid INTEGER NOT NULL, name TEXT NOT NULL,
  enabled INTEGER NOT NULL, page INTEGER NOT NULL, ord INTEGER NOT NULL);

CREATE TABLE IF NOT EXISTS binding (
  uid INTEGER PRIMARY KEY, set_uid INTEGER NOT NULL, bid INTEGER NOT NULL,
  label TEXT NOT NULL, enabled INTEGER NOT NULL, ord INTEGER NOT NULL,
  src_kind INTEGER NOT NULL, src_channel INTEGER NOT NULL, src_index INTEGER NOT NULL,
  src_address TEXT NOT NULL, src_arg INTEGER NOT NULL, src_mods INTEGER NOT NULL,
  tgt_kind INTEGER NOT NULL, tgt_object INTEGER NOT NULL, tgt_command TEXT NOT NULL,
  tgt_label TEXT NOT NULL, tgt_sub INTEGER NOT NULL,
  in_min REAL NOT NULL, in_max REAL NOT NULL, invert INTEGER NOT NULL,
  out_min REAL NOT NULL, out_max REAL NOT NULL,
  curve INTEGER NOT NULL, mode INTEGER NOT NULL, takeover INTEGER NOT NULL,
  deadband REAL NOT NULL, smoothing REAL NOT NULL, feedback INTEGER NOT NULL,
  edge INTEGER NOT NULL, threshold REAL NOT NULL, hysteresis REAL NOT NULL,
  rel_scale REAL NOT NULL, toggle_state INTEGER NOT NULL,
  FOREIGN KEY(set_uid) REFERENCES binding_set(uid) ON DELETE CASCADE);

CREATE INDEX IF NOT EXISTS ix_cue_seq ON cue(seq_uid, idx);
CREATE INDEX IF NOT EXISTS ix_lib_chan ON library_channel(library_uid, mode_ord, ord);
CREATE INDEX IF NOT EXISTS ix_preset_pool ON preset(pool, pid);
CREATE INDEX IF NOT EXISTS ix_slot_recipe ON recipe_slot(recipe_uid, ord);
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

    // --- groups (§20.8) ---------------------------------------------------
    {
        Stmt gp(db, "INSERT INTO grp(uid,gid,name,kind,allow_overrides,ord,q_type,"
                    "q_universe,q_addr_from,q_addr_to,q_id_from,q_id_to,q_attr,"
                    "q_group,q_kind,q_sort) "
                    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        Stmt gm(db, "INSERT INTO grp_member(group_uid,fixture,cell,ord) VALUES(?,?,?,?)");
        Stmt gc(db, "INSERT INTO grp_child(group_uid,child_uid,ord) VALUES(?,?,?)");
        Stmt go(db, "INSERT INTO grp_override(group_uid,fixture_uid,included) "
                    "VALUES(?,?,?)");
        Stmt gt(db, "INSERT INTO grp_tag(group_uid,tag,is_query) VALUES(?,?,?)");
        if (!gp.valid() || !gm.valid() || !gc.valid() || !go.valid() || !gt.valid())
            return fail("prepare groups");

        int ord = 0;
        for (const GroupDef& g : eng.groups.all()) {
            const GroupQuery& q = g.query;
            gp.reset();
            gp.bindUid(1, g.uid)
              .bind(2, g.id)
              .bind(3, g.name)
              .bind(4, static_cast<int>(g.kind))
              .bind(5, g.allowOverrides ? 1 : 0)
              .bind(6, ord++)
              .bind(16, q.sortKey);
            // An absent facet stays NULL, so "no filter" and "filter on 0" do
            // not collapse into the same thing.
            if (q.typeName) gp.bind(7, *q.typeName); else gp.bindNull(7);
            if (q.universe) gp.bind(8, *q.universe); else gp.bindNull(8);
            if (q.addressRange) {
                gp.bind(9, q.addressRange->first).bind(10, q.addressRange->second);
            } else {
                gp.bindNull(9).bindNull(10);
            }
            if (q.idRange) {
                gp.bind(11, static_cast<int64_t>(q.idRange->first))
                  .bind(12, static_cast<int64_t>(q.idRange->second));
            } else {
                gp.bindNull(11).bindNull(12);
            }
            if (q.hasAttr) gp.bind(13, static_cast<int>(*q.hasAttr)); else gp.bindNull(13);
            if (q.hasGroup) gp.bind(14, static_cast<int>(*q.hasGroup)); else gp.bindNull(14);
            if (q.hasKind) gp.bind(15, static_cast<int>(*q.hasKind)); else gp.bindNull(15);
            if (!gp.run()) return fail("write group");
            ++rep.groups;

            int i = 0;
            for (const Target& t : g.members) {
                gm.reset();
                gm.bindUid(1, g.uid)
                  .bind(2, static_cast<int64_t>(t.fixture))
                  .bind(3, static_cast<int>(t.cell))
                  .bind(4, i++);
                if (!gm.run()) return fail("write group member");
            }
            i = 0;
            for (Uid c : g.children) {
                gc.reset();
                gc.bindUid(1, g.uid).bindUid(2, c).bind(3, i++);
                if (!gc.run()) return fail("write group child");
            }
            for (Uid u : g.includes) {
                go.reset();
                go.bindUid(1, g.uid).bindUid(2, u).bind(3, 1);
                if (!go.run()) return fail("write group include");
            }
            for (Uid u : g.excludes) {
                go.reset();
                go.bindUid(1, g.uid).bindUid(2, u).bind(3, 0);
                if (!go.run()) return fail("write group exclude");
            }
            for (const auto& tag : g.tags) {
                gt.reset();
                gt.bindUid(1, g.uid).bind(2, tag).bind(3, 0);
                if (!gt.run()) return fail("write group tag");
            }
            for (const auto& tag : q.anyTag) {
                gt.reset();
                gt.bindUid(1, g.uid).bind(2, tag).bind(3, 1);
                if (!gt.run()) return fail("write group query tag");
            }
        }
    }

    // --- presets (§7) -----------------------------------------------------
    {
        Stmt pl(db, "INSERT INTO preset_pool(pid,name,grp,ord) VALUES(?,?,?,?)");
        Stmt pr(db, "INSERT INTO preset(uid,pool,pid,name,grp,generated,"
                    "symbol_override,indicator_override,ord) VALUES(?,?,?,?,?,?,?,?,?)");
        Stmt pv(db, "INSERT INTO preset_value(preset_uid,fixture,cell,attr,kind,value,"
                    "active,ref_uid,ref_attr) VALUES(?,?,?,?,?,?,?,?,?)");
        Stmt pt(db, "INSERT INTO preset_tag(preset_uid,tag) VALUES(?,?)");
        if (!pl.valid() || !pr.valid() || !pv.valid() || !pt.valid())
            return fail("prepare presets");

        int poolOrd = 0;
        for (const PresetPool& pool : eng.presets.pools()) {
            pl.reset();
            pl.bind(1, pool.id).bind(2, pool.name)
              .bind(3, static_cast<int>(pool.group)).bind(4, poolOrd++);
            if (!pl.run()) return fail("write preset pool");

            int ord = 0;
            for (const Preset& p : pool.presets) {
                pr.reset();
                pr.bindUid(1, p.uid)
                  .bind(2, p.pool)
                  .bind(3, p.id)
                  .bind(4, p.name)
                  .bind(5, static_cast<int>(p.group))
                  .bind(6, p.generated ? 1 : 0)
                  .bind(7, p.symbolOverride)
                  .bind(8, p.indicatorOverride)
                  .bind(9, ord++);
                if (!pr.run()) return fail("write preset");
                ++rep.presets;

                for (AttrKey k : p.values.keys()) {
                    const Value* v = p.values.find(k);
                    pv.reset();
                    pv.bindUid(1, p.uid)
                      .bind(2, static_cast<int64_t>(keyFixture(k)))
                      .bind(3, static_cast<int>(keyCell(k)))
                      .bind(4, static_cast<int>(keyAttr(k)))
                      .bind(5, static_cast<int>(v->kind))
                      .bind(6, static_cast<double>(v->v))
                      .bind(7, v->active ? 1 : 0)
                      .bindUid(8, v->preset)
                      .bind(9, static_cast<int>(v->presetAttr));
                    if (!pv.run()) return fail("write preset value");
                }
                for (const auto& tag : p.tags) {
                    pt.reset();
                    pt.bindUid(1, p.uid).bind(2, tag);
                    if (!pt.run()) return fail("write preset tag");
                }
            }
        }
    }

    // --- FX recipes (§4) --------------------------------------------------
    {
        Stmt rc(db, "INSERT INTO recipe(uid,rid,name,speed,size,spread,offset,base_hz,ord)"
                    " VALUES(?,?,?,?,?,?,?,?,?)");
        Stmt rt(db, "INSERT INTO recipe_tag(recipe_uid,tag) VALUES(?,?)");
        Stmt sl(db, "INSERT INTO recipe_slot(recipe_uid,ord,label,attr,role,gen_kind,"
                    "wave,duty,relative,size_mul,speed_mul,phase_offset,centre,blocks,"
                    "wings,groups,offset,source,direction,pivot,gap_on,gap_len,gap_frac,"
                    "gap_nth,gap_hold,gap_place,gap_resume,gap_scope,gap_trans,var_phase,"
                    "var_speed,var_size,var_seed,layer,stack,opacity,muted,soloed) "
                    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,"
                    "?,?,?,?,?,?,?,?)");
        Stmt ss(db, "INSERT INTO recipe_slot_step(slot_id,ord,value,width,attack,decay,"
                    "attack_curve,decay_curve) VALUES(?,?,?,?,?,?,?,?)");
        Stmt sc(db, "INSERT INTO recipe_slot_curve(slot_id,ord,x,y,in_dx,in_dy,out_dx,"
                    "out_dy) VALUES(?,?,?,?,?,?,?,?)");
        Stmt sg(db, "INSERT INTO recipe_slot_gap_seq(slot_id,ord,length) VALUES(?,?,?)");
        if (!rc.valid() || !rt.valid() || !sl.valid() || !ss.valid() || !sc.valid() ||
            !sg.valid())
            return fail("prepare recipes");

        int ord = 0;
        for (const Recipe& r : eng.recipes.all()) {
            rc.reset();
            rc.bindUid(1, r.uid)
              .bind(2, r.id)
              .bind(3, r.name)
              .bind(4, static_cast<double>(r.speed))
              .bind(5, static_cast<double>(r.size))
              .bind(6, static_cast<double>(r.spread))
              .bind(7, static_cast<double>(r.offset))
              .bind(8, static_cast<double>(r.baseHz))
              .bind(9, ord++);
            if (!rc.run()) return fail("write recipe");
            ++rep.recipes;

            for (const auto& tag : r.tags) {
                rt.reset();
                rt.bindUid(1, r.uid).bind(2, tag);
                if (!rt.run()) return fail("write recipe tag");
            }

            int si = 0;
            for (const Slot& s : r.slots) {
                sl.reset();
                // Bind positions written out explicitly: an incrementing
                // counter inside one chained expression is unsequenced, which
                // bit once already.
                sl.bindUid(1, r.uid)
                  .bind(2, si++)
                  .bind(3, s.label)
                  .bind(4, static_cast<int>(s.attr))
                  .bind(5, static_cast<int>(s.role))
                  .bind(6, static_cast<int>(s.gen.kind))
                  .bind(7, static_cast<int>(s.gen.wave))
                  .bind(8, static_cast<double>(s.gen.duty))
                  .bind(9, s.relative ? 1 : 0)
                  .bind(10, static_cast<double>(s.sizeMul))
                  .bind(11, static_cast<double>(s.speedMul))
                  .bind(12, static_cast<double>(s.phaseOffset))
                  .bind(13, static_cast<double>(s.centre))
                  .bind(14, s.mods.blocks)
                  .bind(15, s.mods.wings)
                  .bind(16, static_cast<double>(s.mods.groups))
                  .bind(17, static_cast<double>(s.mods.offset))
                  .bind(18, static_cast<int>(s.source))
                  .bind(19, static_cast<int>(s.direction))
                  .bind(20, static_cast<int>(s.pivot))
                  .bind(21, s.gap.enabled ? 1 : 0)
                  .bind(22, static_cast<double>(s.gap.length))
                  .bind(23, s.gap.lengthIsFraction ? 1 : 0)
                  .bind(24, s.gap.everyNth)
                  .bind(25, static_cast<int>(s.gap.hold))
                  .bind(26, static_cast<int>(s.gap.placement))
                  .bind(27, static_cast<int>(s.gap.resume))
                  .bind(28, static_cast<int>(s.gap.scope))
                  .bind(29, static_cast<double>(s.gap.transition))
                  .bind(30, static_cast<double>(s.variation.phase))
                  .bind(31, static_cast<double>(s.variation.speed))
                  .bind(32, static_cast<double>(s.variation.size))
                  .bind(33, static_cast<int64_t>(s.variation.seed))
                  .bind(34, s.layer)
                  .bind(35, static_cast<int>(s.stack))
                  .bind(36, static_cast<double>(s.opacity))
                  .bind(37, s.muted ? 1 : 0)
                  .bind(38, s.soloed ? 1 : 0);
                if (!sl.run()) return fail("write slot");
                const int64_t slotId = sqlite3_last_insert_rowid(db.raw());

                int i = 0;
                for (const Step& st : s.gen.steps) {
                    ss.reset();
                    ss.bind(1, slotId).bind(2, i++)
                      .bind(3, static_cast<double>(st.value))
                      .bind(4, static_cast<double>(st.width))
                      .bind(5, static_cast<double>(st.attack))
                      .bind(6, static_cast<double>(st.decay))
                      .bind(7, static_cast<double>(st.attackCurve))
                      .bind(8, static_cast<double>(st.decayCurve));
                    if (!ss.run()) return fail("write slot step");
                }
                i = 0;
                for (const CurvePoint& cp : s.gen.curve) {
                    sc.reset();
                    sc.bind(1, slotId).bind(2, i++)
                      .bind(3, static_cast<double>(cp.x))
                      .bind(4, static_cast<double>(cp.y))
                      .bind(5, static_cast<double>(cp.inDx))
                      .bind(6, static_cast<double>(cp.inDy))
                      .bind(7, static_cast<double>(cp.outDx))
                      .bind(8, static_cast<double>(cp.outDy));
                    if (!sc.run()) return fail("write slot curve");
                }
                i = 0;
                for (float len : s.gap.sequence) {
                    sg.reset();
                    sg.bind(1, slotId).bind(2, i++).bind(3, static_cast<double>(len));
                    if (!sg.run()) return fail("write slot gap");
                }
            }
        }
    }

    // --- fixture library (§1.4) -------------------------------------------
    {
        Stmt lb(db, "INSERT INTO library(uid,lid,name,source,edited,manufacturer,"
                    "fixture_name,short_name,type_id,description,package,ord) "
                    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
        Stmt lt(db, "INSERT INTO library_tag(library_uid,tag) VALUES(?,?)");
        Stmt lg(db, "INSERT INTO library_geometry(library_uid,ord,parent,name,kind,"
                    "referenced,dmx_offset) VALUES(?,?,?,?,?,?,?)");
        Stmt lm(db, "INSERT INTO library_mode(library_uid,ord,name,root) "
                    "VALUES(?,?,?,?)");
        Stmt lc(db, "INSERT INTO library_channel(library_uid,mode_ord,ord,attr,geometry,"
                    "coarse,fine,default_value,invert) VALUES(?,?,?,?,?,?,?,?,?)");
        Stmt ls(db, "INSERT INTO library_channel_set(channel_id,ord,name,dmx_from,"
                    "dmx_to,phys_from,phys_to,anchor) VALUES(?,?,?,?,?,?,?,?)");
        Stmt lr(db, "INSERT INTO library_revision(library_uid,ord,text,date) "
                    "VALUES(?,?,?,?)");
        if (!lb.valid() || !lt.valid() || !lg.valid() || !lm.valid() || !lc.valid() ||
            !ls.valid() || !lr.valid())
            return fail("prepare library");

        int ord = 0;
        for (const LibraryEntry& e : eng.library.all()) {
            lb.reset();
            lb.bindUid(1, e.uid)
              .bind(2, e.id)
              .bind(3, e.name)
              .bind(4, e.sourceName)
              .bind(5, e.edited ? 1 : 0)
              .bind(6, e.def.manufacturer)
              .bind(7, e.def.name)
              .bind(8, e.def.shortName)
              .bind(9, e.def.typeId)
              .bind(10, e.def.description)
              .bindBlob(11, e.gdtf)
              .bind(12, ord++);
            if (!lb.run()) return fail("write library entry");
            ++rep.library;

            for (const auto& tag : e.tags) {
                lt.reset();
                lt.bindUid(1, e.uid).bind(2, tag);
                if (!lt.run()) return fail("write library tag");
            }

            // Pre-order, so a parent is always written before its children.
            int gi = 0;
            bool geoFailed = false;
            std::function<void(const std::vector<GeometryNode>&, const std::string&)>
                emit = [&](const std::vector<GeometryNode>& nodes,
                           const std::string& parent) {
                    for (const GeometryNode& n : nodes) {
                        if (geoFailed) return;
                        lg.reset();
                        lg.bindUid(1, e.uid)
                          .bind(2, gi++)
                          .bind(3, parent)
                          .bind(4, n.name)
                          .bind(5, static_cast<int>(n.kind))
                          .bind(6, n.referenced)
                          .bind(7, n.dmxOffset);
                        if (!lg.run()) {
                            geoFailed = true;
                            return;
                        }
                        emit(n.children, n.name);
                    }
                };
            emit(e.def.geometries, std::string());
            if (geoFailed) return fail("write library geometry");

            for (size_t mi = 0; mi < e.def.modes.size(); ++mi) {
                const ModeDef& m = e.def.modes[mi];
                lm.reset();
                lm.bindUid(1, e.uid)
                  .bind(2, static_cast<int>(mi))
                  .bind(3, m.name)
                  .bind(4, m.rootGeometry);
                if (!lm.run()) return fail("write library mode");

                for (size_t ci = 0; ci < m.channels.size(); ++ci) {
                    const ChannelDecl& c = m.channels[ci];
                    lc.reset();
                    lc.bindUid(1, e.uid)
                      .bind(2, static_cast<int>(mi))
                      .bind(3, static_cast<int>(ci))
                      .bind(4, static_cast<int>(c.attr))
                      .bind(5, c.geometry)
                      .bind(6, c.coarse)
                      .bind(7, c.fine)
                      .bind(8, static_cast<int>(c.defaultValue))
                      .bind(9, c.invert ? 1 : 0);
                    if (!lc.run()) return fail("write library channel");
                    const int64_t chanId = sqlite3_last_insert_rowid(db.raw());

                    for (size_t si = 0; si < c.sets.size(); ++si) {
                        const ChannelSetDef& st = c.sets[si];
                        ls.reset();
                        ls.bind(1, chanId)
                          .bind(2, static_cast<int>(si))
                          .bind(3, st.name)
                          .bind(4, static_cast<int>(st.dmxFrom))
                          .bind(5, static_cast<int>(st.dmxTo))
                          .bind(6, static_cast<double>(st.physFrom))
                          .bind(7, static_cast<double>(st.physTo))
                          .bind(8, st.anchor ? 1 : 0);
                        if (!ls.run()) return fail("write library channel set");
                    }
                }
            }

            for (size_t ri = 0; ri < e.def.revisions.size(); ++ri) {
                lr.reset();
                lr.bindUid(1, e.uid)
                  .bind(2, static_cast<int>(ri))
                  .bind(3, e.def.revisions[ri].text)
                  .bind(4, e.def.revisions[ri].date);
                if (!lr.run()) return fail("write library revision");
            }
        }
    }

    // --- binding sets (§12.6) ---------------------------------------------
    {
        Stmt bs(db, "INSERT INTO binding_set(uid,bid,name,enabled,page,ord) "
                    "VALUES(?,?,?,?,?,?)");
        Stmt bn(db, "INSERT INTO binding(uid,set_uid,bid,label,enabled,ord,src_kind,"
                    "src_channel,src_index,src_address,src_arg,src_mods,tgt_kind,"
                    "tgt_object,tgt_command,tgt_label,tgt_sub,in_min,in_max,invert,"
                    "out_min,out_max,curve,mode,takeover,deadband,smoothing,feedback,"
                    "edge,threshold,hysteresis,rel_scale,toggle_state) "
                    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,"
                    "?,?,?,?)");
        if (!bs.valid() || !bn.valid()) return fail("prepare bindings");

        int ord = 0;
        for (const BindingSet& s : eng.bindings.sets()) {
            bs.reset();
            bs.bindUid(1, s.uid)
              .bind(2, s.id)
              .bind(3, s.name)
              .bind(4, s.enabled ? 1 : 0)
              .bind(5, s.page)
              .bind(6, ord++);
            if (!bs.run()) return fail("write binding set");
            ++rep.bindingSets;

            int bi = 0;
            for (const Binding& b : s.bindings) {
                bn.reset();
                bn.bindUid(1, b.uid)
                  .bindUid(2, s.uid)
                  .bind(3, b.id)
                  .bind(4, b.label)
                  .bind(5, b.enabled ? 1 : 0)
                  .bind(6, bi++)
                  .bind(7, static_cast<int>(b.source.kind))
                  .bind(8, b.source.channel)
                  .bind(9, b.source.index)
                  .bind(10, b.source.address)
                  .bind(11, b.source.argIndex)
                  .bind(12, static_cast<int>(b.source.mods))
                  .bind(13, static_cast<int>(b.target.kind))
                  .bindUid(14, b.target.object)
                  .bind(15, b.target.command)
                  .bind(16, b.target.label)
                  .bind(17, b.target.subIndex)
                  .bind(18, static_cast<double>(b.map.inMin))
                  .bind(19, static_cast<double>(b.map.inMax))
                  .bind(20, b.map.invert ? 1 : 0)
                  .bind(21, static_cast<double>(b.map.outMin))
                  .bind(22, static_cast<double>(b.map.outMax))
                  .bind(23, static_cast<int>(b.map.curve))
                  .bind(24, static_cast<int>(b.map.mode))
                  .bind(25, static_cast<int>(b.map.takeover))
                  .bind(26, static_cast<double>(b.map.deadband))
                  .bind(27, static_cast<double>(b.map.smoothing))
                  .bind(28, b.map.feedback ? 1 : 0)
                  .bind(29, static_cast<int>(b.map.edge))
                  .bind(30, static_cast<double>(b.map.threshold))
                  .bind(31, static_cast<double>(b.map.hysteresis))
                  .bind(32, static_cast<double>(b.map.relativeScale))
                  // The toggle's state is the state of the thing it controls,
                  // so it belongs in the show; takeover state does not — a
                  // reloaded show has not had its faders touched yet.
                  .bind(33, b.toggleState ? 1 : 0);
                if (!bn.run()) return fail("write binding");
                ++rep.bindings;
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

    // --- groups -----------------------------------------------------------
    GroupStore groups(eng.uids);
    {
        Stmt gp(db, "SELECT uid,gid,name,kind,allow_overrides,q_type,q_universe,"
                    "q_addr_from,q_addr_to,q_id_from,q_id_to,q_attr,q_group,q_kind,"
                    "q_sort FROM grp ORDER BY ord");
        if (gp.valid()) {
            while (gp.step()) {
                GroupDef g;
                g.uid = gp.uid(0);
                g.id = gp.i32(1);
                g.name = gp.text(2);
                g.kind = gp.i32(3) == 1 ? GroupKind::Dynamic : GroupKind::Static;
                g.allowOverrides = gp.i32(4) != 0;
                eng.uids.observe(g.uid);

                GroupQuery& q = g.query;
                if (!gp.isNull(5)) q.typeName = gp.text(5);
                if (!gp.isNull(6)) q.universe = gp.i32(6);
                if (!gp.isNull(7) && !gp.isNull(8))
                    q.addressRange = std::make_pair(gp.i32(7), gp.i32(8));
                if (!gp.isNull(9) && !gp.isNull(10))
                    q.idRange = std::make_pair(static_cast<FixtureId>(gp.i64(9)),
                                               static_cast<FixtureId>(gp.i64(10)));
                if (!gp.isNull(11)) q.hasAttr = static_cast<Attr>(gp.i32(11));
                if (!gp.isNull(12)) q.hasGroup = static_cast<Group>(gp.i32(12));
                if (!gp.isNull(13)) q.hasKind = static_cast<Kind>(gp.i32(13));
                q.sortKey = gp.text(14);

                Stmt gm(db, "SELECT fixture,cell FROM grp_member WHERE group_uid=? "
                            "ORDER BY ord");
                gm.bindUid(1, g.uid);
                while (gm.step())
                    g.members.push_back({static_cast<FixtureId>(gm.i64(0)),
                                         static_cast<CellId>(gm.i32(1))});

                Stmt gc(db, "SELECT child_uid FROM grp_child WHERE group_uid=? "
                            "ORDER BY ord");
                gc.bindUid(1, g.uid);
                while (gc.step()) g.children.push_back(gc.uid(0));

                Stmt go(db, "SELECT fixture_uid,included FROM grp_override "
                            "WHERE group_uid=? ORDER BY rowid");
                go.bindUid(1, g.uid);
                while (go.step()) {
                    if (go.i32(1) != 0) g.includes.push_back(go.uid(0));
                    else g.excludes.push_back(go.uid(0));
                }

                Stmt gt(db, "SELECT tag,is_query FROM grp_tag WHERE group_uid=? "
                            "ORDER BY rowid");
                gt.bindUid(1, g.uid);
                while (gt.step()) {
                    if (gt.i32(1) != 0) g.query.anyTag.push_back(gt.text(0));
                    else g.tags.push_back(gt.text(0));
                }

                groups.add(std::move(g));
                ++rep.groups;
            }
        } else {
            // An older file simply has no groups; that is not a failed load.
            rep.warnings.push_back("file has no group table");
        }
    }

    // --- presets ----------------------------------------------------------
    PresetStore presets(eng.uids);
    {
        Stmt pl(db, "SELECT pid,name,grp FROM preset_pool ORDER BY ord");
        if (pl.valid()) {
            while (pl.step()) {
                const int poolId = pl.i32(0);
                presets.ensurePool(poolId, pl.text(1), static_cast<Group>(pl.i32(2)));
            }

            Stmt pr(db, "SELECT uid,pool,pid,name,grp,generated,symbol_override,"
                        "indicator_override FROM preset ORDER BY pool,ord");
            while (pr.step()) {
                Preset p;
                p.uid = pr.uid(0);
                p.pool = pr.i32(1);
                p.id = pr.i32(2);
                p.name = pr.text(3);
                p.group = static_cast<Group>(pr.i32(4));
                p.generated = pr.i32(5) != 0;
                p.symbolOverride = pr.text(6);
                p.indicatorOverride = pr.text(7);
                eng.uids.observe(p.uid);

                Stmt pv(db, "SELECT fixture,cell,attr,kind,value,active,ref_uid,ref_attr "
                            "FROM preset_value WHERE preset_uid=? ORDER BY rowid");
                pv.bindUid(1, p.uid);
                while (pv.step()) {
                    const AttrKey k = makeKey(static_cast<FixtureId>(pv.i64(0)),
                                              static_cast<CellId>(pv.i32(1)),
                                              static_cast<Attr>(pv.i32(2)));
                    Value v;
                    v.kind = static_cast<Value::Kind>(pv.i32(3));
                    v.v = static_cast<float>(pv.dbl(4));
                    v.active = pv.i32(5) != 0;
                    v.preset = pv.uid(6);
                    v.presetAttr = static_cast<Attr>(pv.i32(7));
                    p.values.setKey(k, v);
                }

                Stmt pt(db, "SELECT tag FROM preset_tag WHERE preset_uid=? ORDER BY rowid");
                pt.bindUid(1, p.uid);
                while (pt.step()) p.tags.push_back(pt.text(0));

                presets.restore(std::move(p));
                ++rep.presets;
            }
        } else {
            rep.warnings.push_back("file has no preset table");
        }
    }

    // --- FX recipes -------------------------------------------------------
    RecipeLibrary recipes(eng.uids);
    {
        Stmt rc(db, "SELECT uid,rid,name,speed,size,spread,offset,base_hz "
                    "FROM recipe ORDER BY ord");
        if (rc.valid()) {
            while (rc.step()) {
                Recipe r;
                r.uid = rc.uid(0);
                r.id = rc.i32(1);
                r.name = rc.text(2);
                r.speed = static_cast<float>(rc.dbl(3));
                r.size = static_cast<float>(rc.dbl(4));
                r.spread = static_cast<float>(rc.dbl(5));
                r.offset = static_cast<float>(rc.dbl(6));
                r.baseHz = static_cast<float>(rc.dbl(7));
                eng.uids.observe(r.uid);

                Stmt rt(db, "SELECT tag FROM recipe_tag WHERE recipe_uid=? ORDER BY rowid");
                rt.bindUid(1, r.uid);
                while (rt.step()) r.tags.push_back(rt.text(0));

                Stmt sl(db, "SELECT id,label,attr,role,gen_kind,wave,duty,relative,"
                            "size_mul,speed_mul,phase_offset,centre,blocks,wings,groups,"
                            "offset,source,direction,pivot,gap_on,gap_len,gap_frac,"
                            "gap_nth,gap_hold,gap_place,gap_resume,gap_scope,gap_trans,"
                            "var_phase,var_speed,var_size,var_seed,layer,stack,opacity,"
                            "muted,soloed FROM recipe_slot WHERE recipe_uid=? ORDER BY ord");
                sl.bindUid(1, r.uid);
                while (sl.step()) {
                    const int64_t slotId = sl.i64(0);
                    Slot s;
                    s.label = sl.text(1);
                    s.attr = static_cast<Attr>(sl.i32(2));
                    s.role = static_cast<Role>(sl.i32(3));
                    s.gen.kind = static_cast<GenKind>(sl.i32(4));
                    s.gen.wave = static_cast<Wave>(sl.i32(5));
                    s.gen.duty = static_cast<float>(sl.dbl(6));
                    s.relative = sl.i32(7) != 0;
                    s.sizeMul = static_cast<float>(sl.dbl(8));
                    s.speedMul = static_cast<float>(sl.dbl(9));
                    s.phaseOffset = static_cast<float>(sl.dbl(10));
                    s.centre = static_cast<float>(sl.dbl(11));
                    s.mods.blocks = sl.i32(12);
                    s.mods.wings = sl.i32(13);
                    s.mods.groups = static_cast<float>(sl.dbl(14));
                    s.mods.offset = static_cast<float>(sl.dbl(15));
                    s.source = static_cast<SpreadSource>(sl.i32(16));
                    s.direction = static_cast<Direction>(sl.i32(17));
                    s.pivot = static_cast<BouncePivot>(sl.i32(18));
                    s.gap.enabled = sl.i32(19) != 0;
                    s.gap.length = static_cast<float>(sl.dbl(20));
                    s.gap.lengthIsFraction = sl.i32(21) != 0;
                    s.gap.everyNth = sl.i32(22);
                    s.gap.hold = static_cast<GapHold>(sl.i32(23));
                    s.gap.placement = static_cast<GapPlacement>(sl.i32(24));
                    s.gap.resume = static_cast<GapResume>(sl.i32(25));
                    s.gap.scope = static_cast<GapScope>(sl.i32(26));
                    s.gap.transition = static_cast<float>(sl.dbl(27));
                    s.variation.phase = static_cast<float>(sl.dbl(28));
                    s.variation.speed = static_cast<float>(sl.dbl(29));
                    s.variation.size = static_cast<float>(sl.dbl(30));
                    s.variation.seed = static_cast<uint64_t>(sl.i64(31));
                    s.layer = sl.i32(32);
                    s.stack = static_cast<StackMode>(sl.i32(33));
                    s.opacity = static_cast<float>(sl.dbl(34));
                    s.muted = sl.i32(35) != 0;
                    s.soloed = sl.i32(36) != 0;

                    Stmt ss(db, "SELECT value,width,attack,decay,attack_curve,decay_curve "
                                "FROM recipe_slot_step WHERE slot_id=? ORDER BY ord");
                    ss.bind(1, slotId);
                    while (ss.step()) {
                        Step st;
                        st.value = static_cast<float>(ss.dbl(0));
                        st.width = static_cast<float>(ss.dbl(1));
                        st.attack = static_cast<float>(ss.dbl(2));
                        st.decay = static_cast<float>(ss.dbl(3));
                        st.attackCurve = static_cast<float>(ss.dbl(4));
                        st.decayCurve = static_cast<float>(ss.dbl(5));
                        s.gen.steps.push_back(st);
                    }

                    Stmt sc(db, "SELECT x,y,in_dx,in_dy,out_dx,out_dy FROM "
                                "recipe_slot_curve WHERE slot_id=? ORDER BY ord");
                    sc.bind(1, slotId);
                    while (sc.step()) {
                        CurvePoint cp;
                        cp.x = static_cast<float>(sc.dbl(0));
                        cp.y = static_cast<float>(sc.dbl(1));
                        cp.inDx = static_cast<float>(sc.dbl(2));
                        cp.inDy = static_cast<float>(sc.dbl(3));
                        cp.outDx = static_cast<float>(sc.dbl(4));
                        cp.outDy = static_cast<float>(sc.dbl(5));
                        s.gen.curve.push_back(cp);
                    }

                    Stmt sg(db, "SELECT length FROM recipe_slot_gap_seq WHERE slot_id=? "
                                "ORDER BY ord");
                    sg.bind(1, slotId);
                    while (sg.step()) s.gap.sequence.push_back(static_cast<float>(sg.dbl(0)));

                    r.slots.push_back(std::move(s));
                }
                recipes.add(std::move(r));
                ++rep.recipes;
            }
        } else {
            rep.warnings.push_back("file has no recipe table");
        }
    }

    // --- fixture library --------------------------------------------------
    FixtureLibrary library(eng.uids);
    {
        Stmt lb(db, "SELECT uid,lid,name,source,edited,manufacturer,fixture_name,"
                    "short_name,type_id,description,package FROM library ORDER BY ord");
        if (lb.valid()) {
            while (lb.step()) {
                LibraryEntry e;
                e.uid = lb.uid(0);
                e.id = lb.i32(1);
                e.name = lb.text(2);
                e.sourceName = lb.text(3);
                e.edited = lb.i32(4) != 0;
                e.def.uid = e.uid;
                e.def.manufacturer = lb.text(5);
                e.def.name = lb.text(6);
                e.def.shortName = lb.text(7);
                e.def.typeId = lb.text(8);
                e.def.description = lb.text(9);
                e.gdtf = lb.blob(10);
                eng.uids.observe(e.uid);

                // Geometries come back in pre-order, so attaching each to its
                // parent by name finds a parent that already exists.
                Stmt lg(db, "SELECT parent,name,kind,referenced,dmx_offset FROM "
                            "library_geometry WHERE library_uid=? ORDER BY ord");
                lg.bindUid(1, e.uid);
                while (lg.step()) {
                    GeometryNode n;
                    const std::string parent = lg.text(0);
                    n.name = lg.text(1);
                    n.kind = static_cast<GeometryKind>(lg.i32(2));
                    n.referenced = lg.text(3);
                    n.dmxOffset = lg.i32(4);
                    if (parent.empty()) {
                        e.def.geometries.push_back(std::move(n));
                        continue;
                    }
                    GeometryNode* p = e.def.findGeometry(parent);
                    if (p) p->children.push_back(std::move(n));
                    else {
                        // A parent the file lost: keep the node rather than
                        // drop a geometry, and say so.
                        rep.warnings.push_back("geometry '" + n.name + "' in '" +
                                               e.name + "' lost its parent '" + parent +
                                               "'; attached at the top");
                        e.def.geometries.push_back(std::move(n));
                    }
                }

                Stmt lm(db, "SELECT ord,name,root FROM library_mode WHERE library_uid=? "
                            "ORDER BY ord");
                lm.bindUid(1, e.uid);
                while (lm.step()) {
                    ModeDef m;
                    const int mi = lm.i32(0);
                    m.name = lm.text(1);
                    m.rootGeometry = lm.text(2);

                    Stmt lc(db, "SELECT id,attr,geometry,coarse,fine,default_value,"
                                "invert FROM library_channel WHERE library_uid=? AND "
                                "mode_ord=? ORDER BY ord");
                    lc.bindUid(1, e.uid).bind(2, mi);
                    while (lc.step()) {
                        ChannelDecl c;
                        const int64_t chanId = lc.i64(0);
                        c.attr = static_cast<Attr>(lc.i32(1));
                        c.geometry = lc.text(2);
                        c.coarse = lc.i32(3);
                        c.fine = lc.i32(4);
                        c.defaultValue = static_cast<uint8_t>(lc.i32(5));
                        c.invert = lc.i32(6) != 0;

                        Stmt ls(db, "SELECT name,dmx_from,dmx_to,phys_from,phys_to,"
                                    "anchor FROM library_channel_set WHERE channel_id=? "
                                    "ORDER BY ord");
                        ls.bind(1, chanId);
                        while (ls.step()) {
                            ChannelSetDef st;
                            st.name = ls.text(0);
                            st.dmxFrom = static_cast<uint8_t>(ls.i32(1));
                            st.dmxTo = static_cast<uint8_t>(ls.i32(2));
                            st.physFrom = static_cast<float>(ls.dbl(3));
                            st.physTo = static_cast<float>(ls.dbl(4));
                            st.anchor = ls.i32(5) != 0;
                            c.sets.push_back(std::move(st));
                        }
                        m.channels.push_back(std::move(c));
                    }
                    e.def.modes.push_back(std::move(m));
                }

                Stmt lr(db, "SELECT text,date FROM library_revision WHERE library_uid=? "
                            "ORDER BY ord");
                lr.bindUid(1, e.uid);
                while (lr.step())
                    e.def.revisions.push_back({lr.text(0), lr.text(1)});

                Stmt lt(db, "SELECT tag FROM library_tag WHERE library_uid=? "
                            "ORDER BY rowid");
                lt.bindUid(1, e.uid);
                while (lt.step()) e.tags.push_back(lt.text(0));

                library.restore(std::move(e));
                ++rep.library;
            }
        } else {
            rep.warnings.push_back("file has no fixture library");
        }
    }

    // --- binding sets -----------------------------------------------------
    BindingStore bindings(eng.uids);
    {
        Stmt bs(db, "SELECT uid,bid,name,enabled,page FROM binding_set ORDER BY ord");
        if (bs.valid()) {
            while (bs.step()) {
                BindingSet s;
                s.uid = bs.uid(0);
                s.id = bs.i32(1);
                s.name = bs.text(2);
                s.enabled = bs.i32(3) != 0;
                s.page = bs.i32(4);
                eng.uids.observe(s.uid);

                Stmt bn(db, "SELECT uid,bid,label,enabled,src_kind,src_channel,"
                            "src_index,src_address,src_arg,src_mods,tgt_kind,tgt_object,"
                            "tgt_command,tgt_label,tgt_sub,in_min,in_max,invert,out_min,"
                            "out_max,curve,mode,takeover,deadband,smoothing,feedback,"
                            "edge,threshold,hysteresis,rel_scale,toggle_state FROM "
                            "binding WHERE set_uid=? ORDER BY ord");
                bn.bindUid(1, s.uid);
                while (bn.step()) {
                    Binding b;
                    b.uid = bn.uid(0);
                    b.id = bn.i32(1);
                    b.label = bn.text(2);
                    b.enabled = bn.i32(3) != 0;
                    b.source.kind = static_cast<SourceKind>(bn.i32(4));
                    b.source.channel = bn.i32(5);
                    b.source.index = bn.i32(6);
                    b.source.address = bn.text(7);
                    b.source.argIndex = bn.i32(8);
                    b.source.mods = static_cast<Mods>(bn.i32(9));
                    b.target.kind = static_cast<TargetKind>(bn.i32(10));
                    b.target.object = bn.uid(11);
                    b.target.command = bn.text(12);
                    b.target.label = bn.text(13);
                    b.target.subIndex = bn.i32(14);
                    b.map.inMin = static_cast<float>(bn.dbl(15));
                    b.map.inMax = static_cast<float>(bn.dbl(16));
                    b.map.invert = bn.i32(17) != 0;
                    b.map.outMin = static_cast<float>(bn.dbl(18));
                    b.map.outMax = static_cast<float>(bn.dbl(19));
                    b.map.curve = static_cast<BindCurve>(bn.i32(20));
                    b.map.mode = static_cast<BindMode>(bn.i32(21));
                    b.map.takeover = static_cast<Takeover>(bn.i32(22));
                    b.map.deadband = static_cast<float>(bn.dbl(23));
                    b.map.smoothing = static_cast<float>(bn.dbl(24));
                    b.map.feedback = bn.i32(25) != 0;
                    b.map.edge = static_cast<ThresholdEdge>(bn.i32(26));
                    b.map.threshold = static_cast<float>(bn.dbl(27));
                    b.map.hysteresis = static_cast<float>(bn.dbl(28));
                    b.map.relativeScale = static_cast<float>(bn.dbl(29));
                    b.toggleState = bn.i32(30) != 0;
                    // Takeover state is deliberately NOT restored: a reloaded
                    // show has not had its faders touched, so nothing is
                    // caught until it is.
                    s.bindings.push_back(std::move(b));
                    ++rep.bindings;
                }
                bindings.restoreSet(std::move(s));
                ++rep.bindingSets;
            }
        } else {
            rep.warnings.push_back("file has no binding sets");
        }
    }

    // Everything read without error: now swap it in.
    eng.bindings = std::move(bindings);
    eng.library = std::move(library);
    eng.groups = std::move(groups);
    eng.presets = std::move(presets);
    eng.recipes = std::move(recipes);
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
