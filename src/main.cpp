// duel-engine: runs a two-player Yu-Gi-Oh! duel in ygopro-core and talks JSON lines over stdin/stdout.
// AGPL-3.0-or-later. See README.md for the protocol.
#include <cstdio>
#include <cstring>
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
static const char* VERSION = "0.2.0";

// ------------------------------------------------------------------ card database (BabelCDB cards.cdb)
struct CardRow { uint32_t alias = 0, type = 0, level = 0, attribute = 0; uint64_t race = 0; int32_t atk = 0, def = 0; std::vector<uint16_t> setcodes; };
static std::unordered_map<uint32_t, CardRow> g_cards;

static bool load_cdb(const std::string& path, std::string& err) {
	sqlite3* db = nullptr;
	if(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) { err = "can't open card database"; if(db) sqlite3_close(db); return false; }
	sqlite3_stmt* st = nullptr;
	if(sqlite3_prepare_v2(db, "select id,alias,setcode,type,atk,def,level,race,attribute from datas", -1, &st, nullptr) != SQLITE_OK) {
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
		for(uint8_t p = 0; p < 2; p++) {
			const json& pl = req["players"][p];
			auto add = [&](uint32_t code, uint32_t loc) {
				if(!g_cards.count(code)) { unknown.push_back(code); return; }
				OCG_NewCardInfo i{p, 0, code, p, loc, 0, POS_FACEDOWN_DEFENSE};
				OCG_DuelNewCard(h, &i);
			};
			for(auto& c : pl.value("main", json::array())) add(c.get<uint32_t>(), LOCATION_DECK);
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
					if(t == MSG_RETRY) {
						// The last answer wasn't legal: the core asks the same question again.
						promptType = prompt.value("type", 0);
						send_prompt("retry");
						return;
					}
					if(msg::is_prompt(t)) { promptType = t; prompt = m; prompt["type"] = t; send_prompt("prompt"); }
					else {
						track(m);
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
			if(!duel->create(req, err)) { fail(err); duel.reset(); continue; }
			emit({{"ev", "started"}, {"id", id}});
			duel->pump();
		} else if(cmd == "respond") {
			if(!duel || duel->over) { fail("no duel running"); continue; }
			duel->respond(req);
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
