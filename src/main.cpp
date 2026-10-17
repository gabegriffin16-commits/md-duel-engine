// duel-engine: runs a two-player Yu-Gi-Oh! duel in ygopro-core and talks JSON lines over stdin/stdout.
// AGPL-3.0-or-later. See README.md for the protocol.
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include "json.hpp"
#include "miniz.h"
#include "sqlite3.h"
#include "ocgapi.h"
#include "ocgapi_constants.h"
#include "messages.h"

using json = nlohmann::json;
static const char* VERSION = "0.4.0";

// ------------------------------------------------------------------ card database (BabelCDB cards.cdb)
struct CardRow { uint32_t alias = 0, type = 0, level = 0, attribute = 0; uint64_t race = 0; int32_t atk = 0, def = 0; std::vector<uint16_t> setcodes;
	uint64_t setcode = 0; uint32_t ot = 0; std::string name, desc; std::vector<std::string> strs; };
static std::unordered_map<uint32_t, CardRow> g_cards;

static bool load_cdb(const std::string& path, std::string& err) {
	sqlite3* db = nullptr;
	if(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) { err = "can't open card database"; if(db) sqlite3_close(db); return false; }
	sqlite3_stmt* st = nullptr;
	if(sqlite3_prepare_v2(db, "select d.id,d.alias,d.setcode,d.type,d.atk,d.def,d.level,d.race,d.attribute,d.ot,t.name,t.desc,t.str1,t.str2,t.str3,t.str4,t.str5,t.str6,t.str7,t.str8,t.str9,t.str10,t.str11,t.str12,t.str13,t.str14,t.str15,t.str16 from datas d left join texts t on t.id=d.id", -1, &st, nullptr) != SQLITE_OK) {
		err = sqlite3_errmsg(db); sqlite3_close(db); return false;
	}
	while(sqlite3_step(st) == SQLITE_ROW) {
		CardRow r;
		uint32_t id = (uint32_t)sqlite3_column_int64(st, 0);
		r.alias = (uint32_t)sqlite3_column_int64(st, 1);
		uint64_t sc = (uint64_t)sqlite3_column_int64(st, 2);
		for(int i = 0; i < 4; i++) { uint16_t s = (sc >> (16 * i)) & 0xffff; if(s) r.setcodes.push_back(s); }
		r.setcodes.push_back(0);
		r.type = (uint32_t)sqlite3_column_int64(st, 3);
		r.atk = (int32_t)sqlite3_column_int64(st, 4);
		r.def = (int32_t)sqlite3_column_int64(st, 5);
		r.level = (uint32_t)sqlite3_column_int64(st, 6);
		r.race = (uint64_t)sqlite3_column_int64(st, 7);
		r.attribute = (uint32_t)sqlite3_column_int64(st, 8);
		r.setcode = sc;
		r.ot = (uint32_t)sqlite3_column_int64(st, 9);
		auto txt = [&](int c) { const unsigned char* v = sqlite3_column_text(st, c); return v ? std::string((const char*)v) : std::string(); };
		r.name = txt(10); r.desc = txt(11);
		for(int i = 0; i < 16; i++) r.strs.push_back(txt(12 + i));
		while(!r.strs.empty() && r.strs.back().empty()) r.strs.pop_back();
		g_cards[id] = std::move(r);
	}
	sqlite3_finalize(st);
	sqlite3_close(db);
	return true;
}
static void card_reader(void*, uint32_t code, OCG_CardData* out) {
	static uint16_t zero = 0;
	std::memset(out, 0, sizeof(*out));
	out->code = code;
	out->setcodes = &zero;
	auto it = g_cards.find(code);
	if(it == g_cards.end()) return;
	const CardRow& r = it->second;
	out->alias = r.alias;
	out->setcodes = const_cast<uint16_t*>(r.setcodes.data());
	out->type = r.type;
	out->level = r.level & 0xff;
	out->lscale = (r.level >> 24) & 0xff;
	out->rscale = (r.level >> 16) & 0xff;
	out->attribute = r.attribute;
	out->race = r.race;
	out->attack = r.atk;
	if(r.type & TYPE_LINK) { out->defense = 0; out->link_marker = (uint32_t)r.def; }
	else { out->defense = r.def; out->link_marker = 0; }
}
static void card_reader_done(void*, OCG_CardData*) {}

// ------------------------------------------------------------------ scripts (ProjectIgnis CardScripts zip or folder)
static mz_zip_archive g_zip;
static std::vector<char> g_zip_data;
static bool g_zip_open = false;
static std::string g_script_dir;
static std::unordered_map<std::string, uint32_t> g_zip_index;

static int score_folder(const std::string& path) {
	// Prefer official scripts over pre-release / unofficial when a name exists in several folders.
	if(path.find("/official/") != std::string::npos) return 0;
	if(path.find("/pre-release/") != std::string::npos) return 2;
	if(path.find("/unofficial/") != std::string::npos) return 3;
	if(path.find("/pre-errata/") != std::string::npos || path.find("/goat/") != std::string::npos || path.find("/rush/") != std::string::npos || path.find("/skill/") != std::string::npos) return 9;
	return 1;
}
static std::string base_name(const std::string& p) { size_t s = p.find_last_of("/\\"); return s == std::string::npos ? p : p.substr(s + 1); }

static bool open_scripts(const std::string& path, std::string& err) {
	if(path.size() > 4 && path.substr(path.size() - 4) == ".zip") {
		std::memset(&g_zip, 0, sizeof(g_zip));
		std::ifstream zf(std::filesystem::u8path(path), std::ios::binary);
		if(!zf) { err = "can't open scripts zip"; return false; }
		g_zip_data.assign(std::istreambuf_iterator<char>(zf), std::istreambuf_iterator<char>());
		if(!mz_zip_reader_init_mem(&g_zip, g_zip_data.data(), g_zip_data.size(), 0)) { err = "scripts zip is damaged"; return false; }
		g_zip_open = true;
		std::map<std::string, int> best;
		uint32_t n = mz_zip_reader_get_num_files(&g_zip);
		for(uint32_t i = 0; i < n; i++) {
			char fn[512];
			mz_zip_reader_get_filename(&g_zip, i, fn, sizeof(fn));
			std::string f = fn;
			if(f.size() < 5 || f.substr(f.size() - 4) != ".lua") continue;
			std::string b = base_name(f);
			int sc = score_folder(f);
			auto it = best.find(b);
			if(it == best.end() || sc < it->second) { best[b] = sc; g_zip_index[b] = i; }
		}
		if(g_zip_index.empty()) { err = "no scripts in zip"; return false; }
		return true;
	}
	g_script_dir = path;
	return true;
}
static bool read_script(const std::string& name, std::string& out) {
	if(g_zip_open) {
		auto it = g_zip_index.find(name);
		if(it == g_zip_index.end()) return false;
		size_t sz = 0;
		void* p = mz_zip_reader_extract_to_heap(&g_zip, it->second, &sz, 0);
		if(!p) return false;
		out.assign((const char*)p, sz);
		mz_free(p);
		return true;
	}
	for(const char* s : {"official/", "", "pre-release/", "unofficial/"}) {
		std::ifstream f(std::filesystem::u8path(g_script_dir + "/" + s + name), std::ios::binary);
		if(f) { std::stringstream ss; ss << f.rdbuf(); out = ss.str(); return true; }
	}
	return false;
}
static std::unordered_map<std::string, std::string> g_script_cache;
static int script_reader(void*, OCG_Duel duel, const char* name) {
	std::string b = base_name(name);
	auto it = g_script_cache.find(b);
	if(it == g_script_cache.end()) { std::string src; if(!read_script(b, src)) src.clear(); it = g_script_cache.emplace(b, std::move(src)).first; }
	if(it->second.empty()) return 0;
	return OCG_LoadScript(duel, it->second.data(), (uint32_t)it->second.size(), name);
}
static void log_handler(void*, const char* text, int type) {
	json j; j["ev"] = "log"; j["type"] = type; j["text"] = text ? text : "";
	std::cout << j.dump(-1, ' ', false, json::error_handler_t::replace) << "\n";
}

static std::string lower(std::string s) { for(auto& ch : s) ch = (char)std::tolower((unsigned char)ch); return s; }
static json card_json(uint32_t code, const CardRow& r) {
	json j = {{"code", code}, {"name", r.name}, {"desc", r.desc}, {"type", r.type}, {"attribute", r.attribute}, {"race", r.race},
		{"level", r.level & 0xff}, {"lscale", (r.level >> 24) & 0xff}, {"rscale", (r.level >> 16) & 0xff}, {"setcode", r.setcode}};
	if(r.type & TYPE_LINK) { j["atk"] = r.atk; j["link"] = r.level & 0xff; j["linkMarker"] = r.def; }
	else { j["atk"] = r.atk; j["def"] = r.def; }
	if(r.alias) j["alias"] = r.alias;
	if(!r.strs.empty()) j["strings"] = r.strs;
	return j;
}

// ------------------------------------------------------------------ dev layouts (test setups, local duels only)
// A card given as a code or a name: exact name (case-insensitive) first, else the shortest name containing it.
static uint32_t resolve_card(const json& v, std::string& err) {
	if(v.is_number()) { uint32_t c = v.get<uint32_t>(); if(g_cards.count(c)) return c; err = "unknown card code " + std::to_string(c); return 0; }
	if(!v.is_string()) { err = "card missing"; return 0; }
	std::string q = lower(v.get<std::string>());
	uint32_t best = 0; size_t bestLen = SIZE_MAX;
	std::vector<std::pair<size_t, std::string>> near;
	for(auto& [code, r] : g_cards) {
		if(r.alias || r.name.empty()) continue;
		std::string n = lower(r.name);
		if(n == q) return code;
		if(n.find(q) != std::string::npos) {
			if(n.size() < bestLen) { bestLen = n.size(); best = code; }
			near.push_back({n.size(), r.name});
		}
	}
	if(best && near.size() <= 3) return best;   // a short part of one name is fine; a vague one lists the choices
	if(near.empty()) { err = "no card named \"" + v.get<std::string>() + "\""; return 0; }
	std::sort(near.begin(), near.end());
	err = "\"" + v.get<std::string>() + "\" matches " + std::to_string(near.size()) + " cards:";
	for(size_t i = 0; i < near.size() && i < 5; i++) err += (i ? ", " : " ") + near[i].second;
	return 0;
}
static bool is_extra_type(uint32_t type) { return (type & (TYPE_FUSION | TYPE_SYNCHRO | TYPE_XYZ | TYPE_LINK)) != 0; }
// "atk" / "def" / "set" / "up" or a POS_* number; set = face-down (Defense for monsters).
static uint32_t pos_of(const json& v, uint32_t def) {
	if(v.is_number()) return v.get<uint32_t>();
	if(!v.is_string()) return def;
	std::string s = lower(v.get<std::string>());
	if(s == "atk" || s == "up" || s == "attack") return POS_FACEUP_ATTACK;
	if(s == "def" || s == "defense") return POS_FACEUP_DEFENSE;
	if(s == "set" || s == "down") return POS_FACEDOWN_DEFENSE;
	return def;
}

// The Lua that builds a layout the way puzzles do (Debug.ReloadFieldBegin / AddCard / ReloadFieldEnd), plus the
// layout's actions (summons, draws, damage...) as an effect that runs them at the start of the turn player's Main
// Phase 1, so they happen for real (messages, animations, triggers). Layout:
//   {"turn":0|1, "players":[{lp, deck[], extra[], hand[], grave[], removed[], mzone[], szone[]} x2], "actions":[...]}
//   cards are codes or names; mzone / szone entries {card|code, seq (else its index), pos, materials[], counters[],
//   owner, equippedTo{con,seq}}; null = empty zone. actions: {"do":"summon", player, card, pos, materials[],
//   "how":"special"|"normal"}, {"do":"draw", player, count}, {"do":"damage"|"recover", player, amount},
//   {"do":"destroy"|"tograve"|"banish"|"tohand", player, card} (a card that player controls on the field).
static bool layout_lua(const json& L, std::string& out, std::string& err) {
	std::ostringstream o;
	auto code_of = [&](const json& e) -> uint32_t {
		if(e.is_object()) return resolve_card(e.contains("card") ? e["card"] : e.value("code", json()), err);
		return resolve_card(e, err);
	};
	o << "local M,S,A={},{},{}\n";
	o << "Debug.ReloadFieldBegin(" << (uint64_t)(DUEL_MODE_MR5 | DUEL_ATTACK_FIRST_TURN) << ",0)\n";
	const json& pls = L.value("players", json::array());
	bool emz[2] = {false, false};   // the two Extra Monster Zones are shared: player 0's 5 / 6 are player 1's 6 / 5
	for(int p = 0; p < 2; p++) {
		const json pl = p < (int)pls.size() ? pls[p] : json::object();
		o << "Debug.SetPlayerInfo(" << p << "," << pl.value("lp", 8000) << ",0,1)\n";
		auto pile = [&](const char* key, int loc, uint32_t pos) -> bool {
			for(auto& e : pl.value(key, json::array())) {
				if(e.is_null()) continue;
				uint32_t c = code_of(e); if(!c) return false;
				uint32_t ps = e.is_object() ? pos_of(e.value("pos", json()), pos) : pos;
				o << "Debug.AddCard(" << c << "," << p << "," << p << "," << loc << ",0," << ps << ")\n";
			}
			return true;
		};
		if(!pile("deck", LOCATION_DECK, POS_FACEDOWN_DEFENSE) || !pile("extra", LOCATION_EXTRA, 0) ||
		   !pile("hand", LOCATION_HAND, POS_FACEDOWN_DEFENSE) || !pile("grave", LOCATION_GRAVE, POS_FACEUP_ATTACK) ||
		   !pile("removed", LOCATION_REMOVED, POS_FACEUP_ATTACK)) return false;
		const json mz = pl.value("mzone", json::array());
		for(size_t i = 0; i < mz.size(); i++) {
			const json& e = mz[i];
			if(e.is_null()) continue;
			uint32_t c = code_of(e); if(!c) return false;
			int seq = e.is_object() ? e.value("seq", (int)i) : (int)i;
			if(seq < 0 || seq > 6) { err = "monster zones are 0-6"; return false; }
			if(seq >= 5) {
				int k = p == 0 ? seq - 5 : 6 - seq;
				if(emz[k]) { err = "both players have a monster in the same Extra Monster Zone"; return false; }
				emz[k] = true;
			}
			uint32_t ps = e.is_object() ? pos_of(e.value("pos", json()), POS_FACEUP_ATTACK) : POS_FACEUP_ATTACK;
			int owner = e.is_object() ? e.value("owner", p) : p;
			o << "M[" << p * 10 + seq << "]=Debug.AddCard(" << c << "," << owner << "," << p << "," << LOCATION_MZONE << "," << seq << "," << ps << ",true)\n";
			if(e.is_object()) {
				for(auto& m : e.value("materials", json::array())) {
					uint32_t mc = resolve_card(m, err); if(!mc) return false;
					o << "Debug.AddCard(" << mc << "," << p << "," << p << "," << LOCATION_MZONE << "," << seq << "," << POS_FACEUP_ATTACK << ")\n";
				}
				for(auto& ct : e.value("counters", json::array()))
					o << "if M[" << p * 10 + seq << "] then Debug.PreAddCounter(M[" << p * 10 + seq << "]," << ct.value("type", 0) << "," << ct.value("count", 0) << ") end\n";
			}
		}
		const json sz = pl.value("szone", json::array());
		for(size_t i = 0; i < sz.size(); i++) {
			const json& e = sz[i];
			if(e.is_null()) continue;
			uint32_t c = code_of(e); if(!c) return false;
			int seq = e.is_object() ? e.value("seq", (int)i) : (int)i;
			uint32_t ps = e.is_object() ? pos_of(e.value("pos", json()), POS_FACEUP_ATTACK) : POS_FACEUP_ATTACK;
			if(ps == POS_FACEDOWN_DEFENSE) ps = POS_FACEDOWN_ATTACK;   // a Set Spell / Trap
			int owner = e.is_object() ? e.value("owner", p) : p;
			o << "S[" << p * 10 + seq << "]=Debug.AddCard(" << c << "," << owner << "," << p << "," << LOCATION_SZONE << "," << seq << "," << ps << ")\n";
		}
		for(size_t i = 0; i < sz.size(); i++) {   // equip cards, once every monster is there
			const json& e = sz[i];
			if(!e.is_object() || !e.contains("equippedTo")) continue;
			int seq = e.value("seq", (int)i);
			const json& t = e["equippedTo"];
			int k = t.value("con", 0) * 10 + t.value("seq", 0);
			o << "if S[" << p * 10 + seq << "] and M[" << k << "] then Debug.PreEquip(S[" << p * 10 + seq << "],M[" << k << "]) end\n";
		}
	}
	// cards the actions summon wait in the hand / Extra Deck
	const json acts = L.value("actions", json::array());
	std::ostringstream run;
	for(size_t i = 0; i < acts.size(); i++) {
		const json& a = acts[i];
		std::string d = a.value("do", "");
		int p = a.value("player", 0) & 1;
		if(d == "summon") {
			uint32_t c = code_of(a); if(!c) return false;
			const CardRow& r = g_cards[c];
			bool ex = is_extra_type(r.type);
			o << "A[" << i << "]=Debug.AddCard(" << c << "," << p << "," << p << "," << (ex ? LOCATION_EXTRA : LOCATION_HAND) << ",0," << POS_FACEDOWN_DEFENSE << ")\n";
			uint32_t ps = pos_of(a.value("pos", json()), POS_FACEUP_ATTACK);
			if(a.value("how", "") == "normal")
				run << "Duel.Summon(A[" << i << "],true,nil)\n";
			else {
				const char* st = (r.type & TYPE_FUSION) ? "SUMMON_TYPE_FUSION" : (r.type & TYPE_SYNCHRO) ? "SUMMON_TYPE_SYNCHRO" :
					(r.type & TYPE_XYZ) ? "SUMMON_TYPE_XYZ" : (r.type & TYPE_LINK) ? "SUMMON_TYPE_LINK" :
					((r.type & TYPE_RITUAL) && (r.type & TYPE_MONSTER)) ? "SUMMON_TYPE_RITUAL" : "SUMMON_TYPE_SPECIAL";
				run << "if Duel.SpecialSummon(A[" << i << "]," << st << "," << p << "," << p << ",true,false," << ps << ")>0 then A[" << i << "]:CompleteProcedure()\n";
				for(auto& m : a.value("materials", json::array())) {
					uint32_t mc = resolve_card(m, err); if(!mc) return false;
					run << " Debug.AddCard(" << mc << "," << p << "," << p << "," << LOCATION_MZONE << ",A[" << i << "]:GetSequence()," << POS_FACEUP_ATTACK << ")\n";
				}
				run << "end\n";
			}
		} else if(d == "draw") run << "Duel.Draw(" << p << "," << a.value("count", 1) << ",REASON_EFFECT)\n";
		else if(d == "damage") run << "Duel.Damage(" << p << "," << a.value("amount", 0) << ",REASON_EFFECT)\n";
		else if(d == "recover") run << "Duel.Recover(" << p << "," << a.value("amount", 0) << ",REASON_EFFECT)\n";
		else if(d == "destroy" || d == "tograve" || d == "banish" || d == "tohand") {
			uint32_t c = code_of(a); if(!c) return false;
			std::string f = d == "destroy" ? "Duel.Destroy(g,REASON_EFFECT)" : d == "tograve" ? "Duel.SendtoGrave(g,REASON_EFFECT)" :
				d == "banish" ? "Duel.Remove(g,POS_FACEUP,REASON_EFFECT)" : "Duel.SendtoHand(g,nil,REASON_EFFECT)";
			run << "do local g=Duel.GetMatchingGroup(Card.IsCode," << p << ",LOCATION_ONFIELD,0,nil," << c << "):GetFirst() if g then " << f << " end end\n";
		} else { err = "unknown action " + d; return false; }
	}
	o << "Debug.ReloadFieldEnd()\n";
	if(!acts.empty()) {
		int tp = L.value("turn", 0) & 1;
		o << "local e=Effect.GlobalEffect()\ne:SetType(EFFECT_TYPE_FIELD+EFFECT_TYPE_CONTINUOUS)\ne:SetCode(EVENT_PHASE_START+PHASE_MAIN1)\n"
		  << "e:SetCondition(function() return Duel.GetTurnPlayer()==" << tp << " end)\n"
		  << "e:SetOperation(function(e)\ne:Reset()\n" << run.str() << "end)\nDuel.RegisterEffect(e,0)\n";
	}
	out = o.str();
	return true;
}

// An answer that lets the duel go on without anyone deciding (used to play through turns a layout skips).
static json default_answer(const json& pr) {
	std::string t = pr.value("t", "");
	auto first_n = [](int n) { json a = json::array(); for(int i = 0; i < n; i++) a.push_back(i); return a; };
	if(t == "select_idlecmd") return {{"act", pr.value("end", 0) ? "end" : "battle"}};
	if(t == "select_battlecmd") return {{"act", pr.value("end", 0) ? "end" : "main2"}};
	if(t == "select_effectyn" || t == "select_yesno") return {{"yes", false}};
	if(t == "select_option" || t == "announce_number") return {{"i", 0}};
	if(t == "select_chain") return {{"i", pr.value("forced", 0) ? 0 : -1}};
	if(t == "select_card" || t == "select_tribute") return {{"picks", first_n(std::max(1, pr.value("min", 1)))}};
	if(t == "select_sum") return {{"picks", first_n((int)pr.value("cards", json::array()).size())}};
	if(t == "select_unselect_card") return pr.value("finishable", 0) ? json{{"done", true}} : json{{"i", 0}};
	if(t == "select_place" || t == "select_disfield") {
		uint32_t flag = pr.value("flag", 0u); int p = pr.value("player", 0);
		json places = json::array();
		for(int b = 0; b < 32 && (int)places.size() < std::max(1, pr.value("count", 1)); b++) {
			if(flag & (1u << b)) continue;
			int side = b >= 16 ? 1 - p : p, k = b & 15;
			if(k == 7 || k > 13) continue;
			places.push_back({{"con", side}, {"loc", k < 8 ? LOCATION_MZONE : LOCATION_SZONE}, {"seq", k < 8 ? k : k - 8}});
		}
		return {{"places", places}};
	}
	if(t == "select_position") { int ps = pr.value("positions", 1); return {{"pos", ps & -ps}}; }
	return json::object();
}

// ------------------------------------------------------------------ output
static void emit(const json& j) { std::cout << j.dump(-1, ' ', false, json::error_handler_t::replace) << "\n"; std::cout.flush(); }
static void fail(const std::string& why) { emit({{"ev", "error"}, {"error", why}}); }

// ------------------------------------------------------------------ board snapshots (from the core's queries)
static const uint32_t QFLAGS = QUERY_CODE | QUERY_POSITION | QUERY_TYPE | QUERY_LEVEL | QUERY_RANK | QUERY_ATTRIBUTE |
	QUERY_RACE | QUERY_ATTACK | QUERY_DEFENSE | QUERY_BASE_ATTACK | QUERY_BASE_DEFENSE | QUERY_EQUIP_CARD |
	QUERY_TARGET_CARD | QUERY_OVERLAY_CARD | QUERY_COUNTERS | QUERY_OWNER | QUERY_STATUS | QUERY_IS_PUBLIC |
	QUERY_LSCALE | QUERY_RSCALE | QUERY_LINK;

template<typename T> static T rd(const uint8_t* p) { T v; std::memcpy(&v, p, sizeof(T)); return v; }
static json loc_json(const uint8_t* p) { return {{"con", p[0]}, {"loc", p[1]}, {"seq", rd<uint32_t>(p + 2)}, {"pos", rd<uint32_t>(p + 6)}}; }

// Cards in one place, in zone/list order; null for an empty zone. Reads the core's per-card records:
// [u16 size][u32 flag][value...] repeated until QUERY_END; an empty zone is a lone u16 0.
static json query_place(OCG_Duel h, uint8_t con, uint32_t loc) {
	OCG_QueryInfo q{QFLAGS, con, loc, 0, 0};
	uint32_t n = 0;
	const uint8_t* b = (const uint8_t*)OCG_DuelQueryLocation(h, &n, &q);
	json out = json::array();
	size_t o = 4;   // total size first
	while(o + 2 <= n) {
		uint16_t sz = rd<uint16_t>(b + o); o += 2;
		if(sz == 0) { out.push_back(nullptr); continue; }
		json c = json::object();
		for(;;) {
			if(o + sz > n || sz < 4) { o = n; break; }
			uint32_t f = rd<uint32_t>(b + o); const uint8_t* v = b + o + 4;
			switch(f) {
			case QUERY_CODE: c["code"] = rd<uint32_t>(v); break;
			case QUERY_POSITION: c["pos"] = rd<uint32_t>(v); break;
			case QUERY_TYPE: c["type"] = rd<uint32_t>(v); break;
			case QUERY_LEVEL: c["level"] = rd<uint32_t>(v); break;
			case QUERY_RANK: c["rank"] = rd<uint32_t>(v); break;
			case QUERY_ATTRIBUTE: c["attribute"] = rd<uint32_t>(v); break;
			case QUERY_RACE: c["race"] = rd<uint64_t>(v); break;
			case QUERY_ATTACK: c["atk"] = rd<int32_t>(v); break;
			case QUERY_DEFENSE: c["def"] = rd<int32_t>(v); break;
			case QUERY_BASE_ATTACK: c["baseAtk"] = rd<int32_t>(v); break;
			case QUERY_BASE_DEFENSE: c["baseDef"] = rd<int32_t>(v); break;
			case QUERY_EQUIP_CARD: if(sz >= 4 + 10 && v[1]) c["equippedTo"] = loc_json(v); break;
			case QUERY_TARGET_CARD: { uint32_t k = rd<uint32_t>(v); json a = json::array(); for(uint32_t i = 0; i < k; i++) a.push_back(loc_json(v + 4 + 10 * i)); if(k) c["targets"] = a; break; }
			case QUERY_OVERLAY_CARD: { uint32_t k = rd<uint32_t>(v); json a = json::array(); for(uint32_t i = 0; i < k; i++) a.push_back(rd<uint32_t>(v + 4 + 4 * i)); if(k) c["materials"] = a; break; }
			case QUERY_COUNTERS: { uint32_t k = rd<uint32_t>(v); json a = json::array(); for(uint32_t i = 0; i < k; i++) { uint32_t x = rd<uint32_t>(v + 4 + 4 * i); a.push_back({{"type", x & 0xffff}, {"count", x >> 16}}); } if(k) c["counters"] = a; break; }
			case QUERY_OWNER: c["owner"] = v[0]; break;
			case QUERY_STATUS: if(rd<uint32_t>(v) & STATUS_DISABLED) c["negated"] = true; break;   // other status bits are bookkeeping
			case QUERY_IS_PUBLIC: c["public"] = v[0] != 0; break;
			case QUERY_LSCALE: c["lscale"] = rd<uint32_t>(v); break;
			case QUERY_RSCALE: c["rscale"] = rd<uint32_t>(v); break;
			case QUERY_LINK: c["link"] = rd<uint32_t>(v); c["linkMarker"] = rd<uint32_t>(v + 4); break;
			}
			o += sz;
			if(f == QUERY_END) break;
			if(o + 2 > n) break;
			sz = rd<uint16_t>(b + o); o += 2;
		}
		out.push_back(std::move(c));
	}
	return out;
}

// What a viewer may know about a card: everything, or (hidden) only where it is and its position.
static json seen(const json& c, uint8_t con, uint32_t loc, int viewer) {
	if(c.is_null() || viewer < 0 || viewer == con) return c;
	bool pub = c.value("public", false);
	bool faceDown = (c.value("pos", 0) & POS_FACEDOWN) != 0;
	bool hide = (loc == LOCATION_HAND && !pub) || (loc == LOCATION_EXTRA && faceDown && !pub) ||
		((loc & (LOCATION_MZONE | LOCATION_SZONE | LOCATION_REMOVED)) && faceDown && !pub);
	if(!hide) return c;
	json h = {{"hidden", true}, {"pos", c.value("pos", 0)}};
	if(c.contains("materials")) h["materials"] = c["materials"];   // Xyz materials are public
	if(c.contains("counters")) h["counters"] = c["counters"];
	return h;
}

// ------------------------------------------------------------------ the duel
struct Duel {
	OCG_Duel h = nullptr;
	int promptType = 0; json prompt;   // the decision the duel is waiting for (promptType 0 = none)
	bool over = false;
	bool split = true;                 // per-player views (online); false = one view of everything (local tests)
	int turn = 0, turnPlayer = 0, phase = 0;
	int64_t lp[2] = {8000, 8000};
	json chain = json::array();        // links of the chain being built / resolved

	// Keeps the duel facts that snapshots carry (the core has no query for these).
	void track(const json& m) {
		const std::string t = m.value("t", "");
		if(t == "new_turn") { turn++; turnPlayer = m.value("player", 0); }
		else if(t == "new_phase") phase = m.value("phase", 0);
		else if(t == "lp_update") lp[m.value("player", 0) & 1] = m.value("lp", 0);
		else if(t == "damage" || t == "pay_lpcost") lp[m.value("player", 0) & 1] -= m.value("amount", 0);
		else if(t == "recover") lp[m.value("player", 0) & 1] += m.value("amount", 0);
		else if(t == "chaining") chain.push_back({{"code", m["code"]}, {"at", m["at"]}, {"player", m["tcon"]}, {"desc", m["desc"]}});
		else if(t == "chain_solving" || t == "chain_negated" || t == "chain_disabled") {
			size_t i = (size_t)m.value("chain", 0);
			if(i >= 1 && i <= chain.size()) chain[i - 1][t == "chain_solving" ? "resolving" : (t == "chain_negated" ? "negated" : "disabled")] = true;
		}
		else if(t == "chain_solved") { size_t i = (size_t)m.value("chain", 0); if(i >= 1 && i <= chain.size()) chain[i - 1]["resolved"] = true; }
		else if(t == "chain_end") chain = json::array();
	}

	json snapshot(int viewer) {
		json players = json::array();
		static const std::pair<const char*, uint32_t> places[] = {{"mzone", LOCATION_MZONE}, {"szone", LOCATION_SZONE},
			{"hand", LOCATION_HAND}, {"grave", LOCATION_GRAVE}, {"removed", LOCATION_REMOVED}, {"extra", LOCATION_EXTRA}};
		for(uint8_t p = 0; p < 2; p++) {
			json pl = json::object();
			for(auto& [name, loc] : places) {
				json cards = query_place(h, p, loc), out = json::array();
				for(auto& c : cards) out.push_back(seen(c, p, loc, viewer));
				pl[name] = std::move(out);
			}
			pl["deck"] = OCG_DuelQueryCount(h, p, LOCATION_DECK);
			pl["lp"] = lp[p];
			players.push_back(std::move(pl));
		}
		return {{"turn", turn}, {"turnPlayer", turnPlayer}, {"phase", phase}, {"players", players}, {"chain", chain}};
	}

	// Sends an event to one player (split) or to everyone (one view).
	void to(int viewer, json ev) { if(split) ev["to"] = viewer; emit(ev); }
	// Board state goes out before each prompt as a diff against what that viewer last got: only the top-level fields
	// and the player places (hand, mzone, grave...) that changed. A viewer's first one, and any after "resync", is
	// "full". Chain windows repeat a lot, so most prompts send nothing.
	json lastState[3];
	static json state_diff(const json& now, const json& before) {
		if(before.is_null()) return now;
		json d = json::object();
		for(auto& [k, v] : now.items()) {
			if(k == "players") {
				json pd = json::object();
				for(int p = 0; p < 2; p++) {
					json one = json::object();
					for(auto& [pk, pv] : v[p].items()) if(before[k][p][pk] != pv) one[pk] = pv;
					if(!one.empty()) pd[std::to_string(p)] = std::move(one);
				}
				if(!pd.empty()) d[k] = std::move(pd);
			} else if(before[k] != v) d[k] = v;
		}
		return d;
	}
	void send_state(int v) {   // v = -1: the one view of everything
		json s = snapshot(v);
		bool full = lastState[v + 1].is_null();
		json d = state_diff(s, lastState[v + 1]);
		lastState[v + 1] = std::move(s);
		if(d.empty()) return;
		json ev = {{"ev", "state"}, {"full", full}, {"state", std::move(d)}};
		if(split) to(v, std::move(ev)); else emit(ev);
	}
	void send_states() {
		if(!split) { send_state(-1); return; }
		for(int v = 0; v < 2; v++) send_state(v);
	}
	void resync(int v) { lastState[v + 1] = nullptr; send_state(split ? v : -1); if(!over && promptType && prompt.value("player", -1) == v) to(v, {{"ev", "prompt"}, {"prompt", split ? msg::prompt_for_player(prompt) : prompt}}); }
	void send_prompt(const char* ev) {
		int p = prompt.value("player", 0);
		send_states();
		if(!split) { emit({{"ev", ev}, {"prompt", prompt}}); return; }
		to(p, {{"ev", ev}, {"prompt", msg::prompt_for_player(prompt)}});
		to(1 - p, {{"ev", "waiting"}, {"player", p}});
	}

	~Duel() { if(h) OCG_DestroyDuel(h); }

	bool dev = false;     // dev commands allowed (local duels)
	int ff = -1;          // playing through to this player's turn without showing it (a layout that starts on turn 2)
	int ffRetries = 0;

	// Starts a duel from a layout (see layout_lua). Decks left out of the layout come from `decks` (shuffled).
	bool create_layout(json L, const json& decks, std::string& err) {
		OCG_DuelOptions o{};
		std::random_device rd; std::mt19937_64 g(((uint64_t)rd() << 32) ^ rd());
		for(auto& s : o.seed) s = g();
		o.flags = DUEL_MODE_MR5 | DUEL_ATTACK_FIRST_TURN;
		o.team1 = {8000, 0, 1}; o.team2 = {8000, 0, 1};
		o.cardReader = card_reader; o.scriptReader = script_reader; o.logHandler = log_handler; o.cardReaderDone = card_reader_done;
		if(OCG_CreateDuel(&h, &o) != OCG_DUEL_CREATION_SUCCESS) { h = nullptr; err = "couldn't create the duel"; return false; }
		for(const char* f : {"constant.lua", "utility.lua"})
			if(!script_reader(nullptr, h, f)) { err = std::string("missing core script ") + f; return false; }
		if(!L.contains("players") || !L["players"].is_array()) L["players"] = json::array({json::object(), json::object()});
		while(L["players"].size() < 2) L["players"].push_back(json::object());
		for(int p = 0; p < 2; p++) {
			json& pl = L["players"][p];
			const json d = decks.is_array() && (int)decks.size() > p ? decks[p] : json::object();
			if(!pl.contains("deck")) {
				std::vector<uint32_t> main;
				for(auto& c : d.value("main", json::array())) main.push_back(c.get<uint32_t>());
				std::shuffle(main.begin(), main.end(), g);
				pl["deck"] = main;
			}
			if(!pl.contains("extra")) pl["extra"] = d.value("extra", json::array());
		}
		std::string src;
		if(!layout_lua(L, src, err)) return false;
		if(!OCG_LoadScript(h, src.data(), (uint32_t)src.size(), "layout.lua")) { err = "the layout script failed (see the log)"; return false; }
		split = L.value("view", std::string("split")) != "all";
		dev = true;
		int tp = L.value("turn", 0) & 1;
		ff = tp == 1 ? 1 : -1;
		ffRetries = 0;
		for(int p = 0; p < 2; p++) lp[p] = L["players"][p].value("lp", 8000);
		OCG_StartDuel(h);
		return true;
	}

	// The duel as a layout: every card where it is now (Deck order kept), LP, the turn player.
	json current_layout() {
		json s = snapshot(-1);
		json L = {{"turn", turnPlayer}, {"players", json::array()}};
		for(int p = 0; p < 2; p++) {
			const json& sp = s["players"][p];
			json pl = {{"lp", lp[p]}};
			json deck = json::array();
			for(auto& c : query_place(h, (uint8_t)p, LOCATION_DECK)) if(!c.is_null()) deck.push_back(c["code"]);
			pl["deck"] = deck;
			for(const char* k : {"hand", "grave", "removed"}) {
				json a = json::array();
				for(auto& c : sp[k]) if(!c.is_null()) a.push_back(c["code"]);
				pl[k] = a;
			}
			json ex = json::array();
			for(auto& c : sp["extra"]) if(!c.is_null()) ex.push_back((c.value("pos", 0) & POS_FACEUP) ? json{{"code", c["code"]}, {"pos", c["pos"]}} : c["code"]);
			pl["extra"] = ex;
			for(const char* k : {"mzone", "szone"}) {
				json a = json::array();
				for(auto& c : sp[k]) {
					if(c.is_null()) { a.push_back(nullptr); continue; }
					json e = {{"code", c["code"]}, {"pos", c["pos"]}, {"owner", c.value("owner", p)}};
					if(c.contains("materials")) e["materials"] = c["materials"];
					if(c.contains("counters")) e["counters"] = c["counters"];
					if(c.contains("equippedTo")) e["equippedTo"] = c["equippedTo"];
					a.push_back(e);
				}
				pl[k] = a;
			}
			L["players"].push_back(pl);
		}
		return L;
	}

	bool create(const json& req, std::string& err) {
		OCG_DuelOptions o{};
		if(req.contains("seed") && req["seed"].is_array() && req["seed"].size() == 4)
			for(int i = 0; i < 4; i++) o.seed[i] = req["seed"][i].get<uint64_t>();
		else { std::random_device rd; std::mt19937_64 g(((uint64_t)rd() << 32) ^ rd()); for(auto& s : o.seed) s = g(); }
		o.flags = DUEL_MODE_MR5;
		uint32_t startLp = req.value("lp", 8000u), hand = req.value("hand", 5u), draw = req.value("draw", 1u);
		o.team1 = {startLp, hand, draw}; o.team2 = {startLp, hand, draw};
		o.cardReader = card_reader; o.scriptReader = script_reader; o.logHandler = log_handler; o.cardReaderDone = card_reader_done;
		if(OCG_CreateDuel(&h, &o) != OCG_DUEL_CREATION_SUCCESS) { h = nullptr; err = "couldn't create the duel"; return false; }
		for(const char* f : {"constant.lua", "utility.lua"})
			if(!script_reader(nullptr, h, f)) { err = std::string("missing core script ") + f; return false; }
		if(!req.contains("players") || !req["players"].is_array() || req["players"].size() != 2) { err = "players: two decks needed"; return false; }
		json unknown = json::array();
		std::mt19937_64 deck_rng(o.seed[0] ^ (o.seed[1] * 0x9E3779B97F4A7C15ull) ^ o.seed[2] ^ o.seed[3]);
		for(uint8_t p = 0; p < 2; p++) {
			const json& pl = req["players"][p];
			auto add = [&](uint32_t code, uint32_t loc) {
				if(!g_cards.count(code)) { unknown.push_back(code); return; }
				OCG_NewCardInfo i{p, 0, code, p, loc, 0, POS_FACEDOWN_DEFENSE};
				OCG_DuelNewCard(h, &i);
			};
			// The core doesn't shuffle the starting Deck (EDOPro's host does it before handing the cards over), so
			// shuffle here, from the duel's seed: same seed, same order.
			std::vector<uint32_t> main;
			for(auto& c : pl.value("main", json::array())) main.push_back(c.get<uint32_t>());
			std::shuffle(main.begin(), main.end(), deck_rng);
			for(uint32_t c : main) add(c, LOCATION_DECK);
			for(auto& c : pl.value("extra", json::array())) add(c.get<uint32_t>(), LOCATION_EXTRA);
		}
		if(!unknown.empty()) emit({{"ev", "warning"}, {"warning", "cards not in the database were left out"}, {"cards", unknown}});
		split = req.value("view", std::string("split")) != "all";
		lp[0] = lp[1] = startLp;
		OCG_StartDuel(h);
		return true;
	}

	// Runs the duel until someone has to decide something or it ends; streams every message.
	void pump() {
		promptType = 0;
		for(int guard = 0; guard < 100000 && !over; guard++) {
			int st = OCG_DuelProcess(h);
			uint32_t n = 0;
			const uint8_t* p = (const uint8_t*)OCG_DuelGetMessage(h, &n);
			for(size_t o = 0; o + 4 <= n;) {
				uint32_t ln = 0; std::memcpy(&ln, p + o, 4);
				if(ln >= 1 && o + 4 + ln <= n) {
					int t = p[o + 4];
					json m = msg::decode(t, p + o + 5, ln - 1);
					if(t == MSG_RELOAD_FIELD) { o += 4 + ln; continue; }   // a layout being set up: the states show it
					if(ff >= 0 && t == MSG_RETRY && ++ffRetries > 20) { ff = -1; fail("couldn't play through to the layout's turn"); }
					if(ff >= 0 && (msg::is_prompt(t) || t == MSG_RETRY)) {
						if(t != MSG_RETRY) { prompt = m; prompt["type"] = t; promptType = t; }
						std::string bytes, err;
						msg::encode_response(promptType, prompt, default_answer(prompt), bytes, err);
						OCG_DuelSetResponse(h, bytes.data(), (uint32_t)bytes.size());
						promptType = 0;
						o += 4 + ln;
						continue;
					}
					if(ff >= 0) {
						track(m);
						if(t == MSG_NEW_TURN && m.value("player", -1) == ff) { ff = -1; send_states(); m["_tracked"] = true; }
						else { o += 4 + ln; continue; }
					}
					if(t == MSG_RETRY) {
						// The last answer wasn't legal: the core asks the same question again.
						promptType = prompt.value("type", 0);
						send_prompt("retry");
						return;
					}
					if(msg::is_prompt(t)) { promptType = t; prompt = m; prompt["type"] = t; send_prompt("prompt"); }
					else {
						if(m.contains("_tracked")) m.erase("_tracked"); else track(m);
						if(!split) emit({{"ev", "msg"}, {"msg", m}});
						else for(int v = 0; v < 2; v++) { json f = msg::for_viewer(m, v); if(!f.is_null()) to(v, {{"ev", "msg"}, {"msg", f}}); }
						if(t == MSG_WIN) { over = true; promptType = 0; send_states(); emit({{"ev", "end"}, {"winner", m.value("player", 2)}, {"reason", m.value("reason", 0)}}); return; }
					}
				}
				o += 4 + ln;
			}
			if(promptType) return;
			if(st == OCG_DUEL_STATUS_END) { if(!over) { over = true; send_states(); emit({{"ev", "end"}, {"winner", 2}, {"reason", -1}}); } return; }
		}
		if(!over && !promptType) fail("duel stopped without a prompt");
	}

	void respond(const json& req) {
		if(!promptType) { fail("no prompt is waiting"); return; }
		int player = req.value("player", -1);
		if(player != prompt.value("player", -2)) { fail("not this player's decision"); return; }
		std::string bytes, err;
		if(!msg::encode_response(promptType, prompt, req.value("answer", json::object()), bytes, err)) { fail(err); send_prompt("retry"); return; }
		OCG_DuelSetResponse(h, bytes.data(), (uint32_t)bytes.size());
		pump();
	}
};

int main(int argc, char** argv) {
	if(argc > 1 && std::string(argv[1]) == "--version") { std::printf("duel-engine %s\n", VERSION); return 0; }
	std::ios::sync_with_stdio(false);
	std::unique_ptr<Duel> duel;
	bool ready = false;
	std::string line;
	while(std::getline(std::cin, line)) {
		if(line.empty() || line == "\r") continue;
		json req;
		try { req = json::parse(line); } catch(...) { fail("bad json"); continue; }
		std::string cmd = req.value("cmd", "");
		json id = req.contains("id") ? req["id"] : json();
		if(cmd == "quit") break;
		if(cmd == "init") {
			std::string err;
			if(!load_cdb(req.value("cdb", ""), err) || !open_scripts(req.value("scripts", ""), err)) { fail(err); continue; }
			ready = true;
			int maj = 0, min = 0; OCG_GetVersion(&maj, &min);
			emit({{"ev", "ready"}, {"id", id}, {"cards", g_cards.size()}, {"scripts", g_zip_index.size()}, {"core", std::to_string(maj) + "." + std::to_string(min)}, {"version", VERSION}});
		} else if(cmd == "start") {
			if(!ready) { fail("init first"); continue; }
			duel = std::make_unique<Duel>();
			std::string err;
			bool ok = req.contains("setup") ? duel->create_layout(req["setup"], req.value("players", json::array()), err) : duel->create(req, err);
			if(ok && req.value("dev", false)) duel->dev = true;
			if(ok && req.contains("setup")) duel->split = req.value("view", std::string("split")) != "all";
			if(!ok) { fail(err); duel.reset(); continue; }
			emit({{"ev", "started"}, {"id", id}});
			duel->send_states();   // the starting board (decks placed, empty hands) before the opening draws
			duel->pump();
		} else if(cmd == "respond") {
			if(!duel || duel->over) { fail("no duel running"); continue; }
			duel->respond(req);
		} else if(cmd == "cards") {
			// Card info for the UI from the same database the rules use: name, text, stats, effect strings.
			json out = json::object();
			for(auto& c : req.value("codes", json::array())) {
				uint32_t code = c.get<uint32_t>();
				auto it = g_cards.find(code);
				if(it == g_cards.end()) continue;
				out[std::to_string(code)] = card_json(code, it->second);
			}
			emit({{"ev", "cards"}, {"id", id}, {"cards", out}});
		} else if(cmd == "search") {
			// Card name search (declaring a card name): case-insensitive substring, official cards first, no tokens.
			std::string q = lower(req.value("text", std::string()));
			size_t limit = req.value("limit", 30u);
			std::vector<std::pair<int, uint32_t>> hits;
			for(auto& [code, r] : g_cards) {
				if(r.alias || (r.type & TYPE_TOKEN) || r.name.empty()) continue;
				std::string n = lower(r.name);
				size_t at = n.find(q);
				if(q.empty() || at == std::string::npos) continue;
				hits.push_back({(at == 0 ? 0 : 1) * 1000 + (int)std::min<size_t>(n.size(), 999), code});
			}
			std::sort(hits.begin(), hits.end());
			json out = json::array();
			for(size_t i = 0; i < hits.size() && i < limit; i++) out.push_back(card_json(hits[i].second, g_cards[hits[i].second]));
			emit({{"ev", "search"}, {"id", id}, {"cards", out}});
		} else if(cmd == "dev") {
			// Test tools (local duels started with "dev":true): change the duel and start it again from the result.
			if(!duel || !duel->dev) { fail("dev commands are off in this duel"); continue; }
			std::string err, op = req.value("op", "");
			json L = op == "load" ? req.value("setup", json::object()) : duel->current_layout();
			if(op == "load") {   // a saved layout; decks it leaves out stay as they are now
				json cur = duel->current_layout();
				if(!L.contains("players")) L["players"] = json::array({json::object(), json::object()});
				for(int p = 0; p < 2 && p < (int)L["players"].size(); p++)
					for(const char* k : {"deck", "extra"}) if(!L["players"][p].contains(k)) L["players"][p][k] = cur["players"][p][k];
			}
			int p = req.value("player", 0) & 1;
			if(req.contains("turn")) L["turn"] = req["turn"].get<int>() & 1;
			json& pl = L["players"][p];
			auto first_free = [](json& zone, int lo, int hi) { for(int i = lo; i <= hi; i++) { while((int)zone.size() <= i) zone.push_back(nullptr); if(zone[i].is_null()) return i; } return -1; };
			if(op == "spawn") {
				uint32_t c = resolve_card(req.value("card", json()), err);
				if(!c) { emit({{"ev", "dev"}, {"ok", false}, {"error", err}}); continue; }
				std::string to = req.value("to", std::string("hand"));
				if(to == "hand" || to == "grave" || to == "removed" || to == "deck" || to == "extra") {
					if(!pl.contains(to)) pl[to] = json::array();
					pl[to].push_back(c);
				} else if(to == "mzone" || to == "szone" || to == "field" || to == "emz") {
					json& zone = pl[to == "mzone" || to == "emz" ? "mzone" : "szone"];
					if(!zone.is_array()) zone = json::array();
					int seq = req.contains("seq") ? req["seq"].get<int>() : to == "field" ? 5 : to == "emz" ? first_free(zone, 5, 6) : first_free(zone, 0, 4);
					if(to == "emz" && seq >= 5) {   // the other player's monster may already be in that one (shared zones)
						json& oz = L["players"][1 - p]["mzone"];
						auto taken = [&](int s) { int os = 11 - s; return oz.is_array() && (int)oz.size() > os && !oz[os].is_null(); };
						if(taken(seq)) seq = (seq == 5 && !taken(6) && (zone.size() <= 6 || zone[6].is_null())) ? 6 : -1;
					}
					if(seq < 0) { emit({{"ev", "dev"}, {"ok", false}, {"error", "no free zone"}}); continue; }
					while((int)zone.size() <= seq) zone.push_back(nullptr);
					json e = {{"code", c}, {"pos", pos_of(req.value("pos", json()), to == "szone" || to == "field" ? POS_FACEUP_ATTACK : POS_FACEUP_ATTACK)}};
					if(req.contains("materials")) e["materials"] = req["materials"];
					zone[seq] = e;
				} else { emit({{"ev", "dev"}, {"ok", false}, {"error", "unknown place " + to}}); continue; }
			} else if(op == "act") {   // summon / draw / damage / recover / destroy... (played for real)
				json a = req.value("action", json::object());
				a["player"] = p;
				L["actions"] = json::array({a});
				if(!req.contains("turn")) L["turn"] = duel->turnPlayer;
			} else if(op == "lp") pl["lp"] = req.value("value", 8000);
			else if(op == "remove") {
				uint32_t c = resolve_card(req.value("card", json()), err);
				if(!c) { emit({{"ev", "dev"}, {"ok", false}, {"error", err}}); continue; }
				bool gone = false;
				for(const char* k : {"mzone", "szone", "hand", "grave", "removed", "extra", "deck"}) {
					json& a = pl[k];
					for(size_t i = 0; i < a.size() && !gone; i++) {
						const json& e = a[i];
						uint32_t ec = e.is_object() ? e.value("code", 0u) : (e.is_number() ? e.get<uint32_t>() : 0u);
						if(ec != c) continue;
						if(std::string(k) == "mzone" || std::string(k) == "szone") a[i] = nullptr; else a.erase(a.begin() + i);
						gone = true;
					}
					if(gone) break;
				}
				if(!gone) { emit({{"ev", "dev"}, {"ok", false}, {"error", "that card isn't there"}}); continue; }
			} else if(op == "clear") {
				for(int q = 0; q < 2; q++) {
					if(req.contains("player") && q != p) continue;
					json& x = L["players"][q];
					for(const char* k : {"mzone", "szone"}) {
						for(auto& e : x[k]) if(!e.is_null()) { x["grave"].push_back(e["code"]); for(auto& m : e.value("materials", json::array())) x["grave"].push_back(m); }
						x[k] = json::array();
					}
				}
			} else if(op != "load" && op != "turn") { emit({{"ev", "dev"}, {"ok", false}, {"error", "unknown op " + op}}); continue; }
			json decks = json::array();
			auto next = std::make_unique<Duel>();
			if(!next->create_layout(L, decks, err)) { emit({{"ev", "dev"}, {"ok", false}, {"error", err}}); continue; }
			next->split = duel->split;
			duel = std::move(next);
			emit({{"ev", "reset"}});
			emit({{"ev", "dev"}, {"ok", true}, {"op", op}});
			duel->send_states();
			duel->pump();
		} else if(cmd == "resync") {
			if(!duel) { fail("no duel running"); continue; }
			duel->resync(req.value("player", 0));
		} else if(cmd == "surrender") {
			if(!duel || duel->over) { fail("no duel running"); continue; }
			duel->over = true; duel->promptType = 0;
			int p = req.value("player", 0);
			emit({{"ev", "end"}, {"winner", 1 - p}, {"reason", "surrender"}});
		} else fail("unknown cmd " + cmd);
	}
	return 0;
}
