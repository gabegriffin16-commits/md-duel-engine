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
static const char* VERSION = "0.1.0";

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

// ------------------------------------------------------------------ the duel
struct Duel {
	OCG_Duel h = nullptr;
	int promptType = 0; json prompt;   // the decision the duel is waiting for (promptType 0 = none)
	bool over = false;

	~Duel() { if(h) OCG_DestroyDuel(h); }

	bool create(const json& req, std::string& err) {
		OCG_DuelOptions o{};
		if(req.contains("seed") && req["seed"].is_array() && req["seed"].size() == 4)
			for(int i = 0; i < 4; i++) o.seed[i] = req["seed"][i].get<uint64_t>();
		else { std::random_device rd; std::mt19937_64 g(((uint64_t)rd() << 32) ^ rd()); for(auto& s : o.seed) s = g(); }
		o.flags = DUEL_MODE_MR5;
		uint32_t lp = req.value("lp", 8000u), hand = req.value("hand", 5u), draw = req.value("draw", 1u);
		o.team1 = {lp, hand, draw}; o.team2 = {lp, hand, draw};
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
						emit({{"ev", "retry"}, {"prompt", prompt}});
						promptType = prompt.value("type", 0);
						return;
					}
					if(msg::is_prompt(t)) { promptType = t; prompt = m; prompt["type"] = t; emit({{"ev", "prompt"}, {"prompt", prompt}}); }
					else {
						emit({{"ev", "msg"}, {"msg", m}});
						if(t == MSG_WIN) { over = true; emit({{"ev", "end"}, {"winner", m.value("player", 2)}, {"reason", m.value("reason", 0)}}); }
					}
				}
				o += 4 + ln;
			}
			if(promptType) return;
			if(st == OCG_DUEL_STATUS_END) { if(!over) { over = true; emit({{"ev", "end"}, {"winner", 2}, {"reason", -1}}); } return; }
		}
		if(!over && !promptType) fail("duel stopped without a prompt");
	}

	void respond(const json& req) {
		if(!promptType) { fail("no prompt is waiting"); return; }
		int player = req.value("player", -1);
		if(player != prompt.value("player", -2)) { fail("not this player's decision"); return; }
		std::string bytes, err;
		if(!msg::encode_response(promptType, prompt, req.value("answer", json::object()), bytes, err)) { fail(err); emit({{"ev", "prompt"}, {"prompt", prompt}}); return; }
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
		} else if(cmd == "surrender") {
			if(!duel || duel->over) { fail("no duel running"); continue; }
			duel->over = true;
			int p = req.value("player", 0);
			emit({{"ev", "end"}, {"winner", 1 - p}, {"reason", "surrender"}});
		} else fail("unknown cmd " + cmd);
	}
	return 0;
}
