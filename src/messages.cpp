// duel-engine: ygopro-core messages <-> JSON. AGPL-3.0-or-later.
//
// Each message's layout is written once as a short schema string, read straight from how ygopro-core writes it
// (pinned commit 38d04c9f). Schema: space-separated fields "name:type" with types u8 u16 u32 i32 u64, "loc"
// (the core's loc_info: controller u8, location u8, sequence u32, position u32) and lists "name:[u8|u32]{...}"
// (a count of that size, then that many items of the inner schema).
#include "messages.h"
#include <cstring>
#include <unordered_map>
#include "ocgapi_constants.h"

namespace msg {

namespace {
struct Spec { const char* name; const char* schema; bool prompt; };

const std::unordered_map<int, Spec>& specs() {
	static const std::unordered_map<int, Spec> m = {
		{MSG_RETRY, {"retry", "", false}},
		{MSG_HINT, {"hint", "type:u8 player:u8 data:u64", false}},
		{MSG_WIN, {"win", "player:u8 reason:u8", false}},
		{MSG_SELECT_BATTLECMD, {"select_battlecmd",
			"player:u8 activate:[u32]{code:u32 con:u8 loc:u8 seq:u32 desc:u64 mode:u8} "
			"attack:[u32]{code:u32 con:u8 loc:u8 seq:u8 direct:u8} main2:u8 end:u8", true}},
		{MSG_SELECT_IDLECMD, {"select_idlecmd",
			"player:u8 summon:[u32]{code:u32 con:u8 loc:u8 seq:u32} spsummon:[u32]{code:u32 con:u8 loc:u8 seq:u32} "
			"repos:[u32]{code:u32 con:u8 loc:u8 seq:u8} mset:[u32]{code:u32 con:u8 loc:u8 seq:u32} "
			"sset:[u32]{code:u32 con:u8 loc:u8 seq:u32} activate:[u32]{code:u32 con:u8 loc:u8 seq:u32 desc:u64 mode:u8} "
			"battle:u8 end:u8 shuffle:u8", true}},
		{MSG_SELECT_EFFECTYN, {"select_effectyn", "player:u8 code:u32 at:loc desc:u64", true}},
		{MSG_SELECT_YESNO, {"select_yesno", "player:u8 desc:u64", true}},
		{MSG_SELECT_OPTION, {"select_option", "player:u8 options:[u8]{desc:u64}", true}},
		{MSG_SELECT_CARD, {"select_card", "player:u8 cancelable:u8 min:u32 max:u32 cards:[u32]{code:u32 at:loc}", true}},
		{MSG_SELECT_CHAIN, {"select_chain",
			"player:u8 spe:u8 forced:u8 hint0:u32 hint1:u32 chains:[u32]{code:u32 at:loc desc:u64 mode:u8}", true}},
		{MSG_SELECT_PLACE, {"select_place", "player:u8 count:u8 flag:u32", true}},
		{MSG_SELECT_POSITION, {"select_position", "player:u8 code:u32 positions:u8", true}},
		{MSG_SELECT_TRIBUTE, {"select_tribute",
			"player:u8 cancelable:u8 min:u32 max:u32 cards:[u32]{code:u32 con:u8 loc:u8 seq:u32 param:u8}", true}},
		{MSG_SORT_CHAIN, {"sort_chain", "player:u8 cards:[u32]{code:u32 con:u8 loc:u32 seq:u32}", true}},
		{MSG_SELECT_COUNTER, {"select_counter",
			"player:u8 ctype:u16 count:u16 cards:[u32]{code:u32 con:u8 loc:u8 seq:u8 have:u16}", true}},
		{MSG_SELECT_SUM, {"select_sum",
			"player:u8 mode:u8 acc:u32 min:u32 max:u32 must:[u32]{code:u32 at:loc param:u32} "
			"cards:[u32]{code:u32 at:loc param:u32}", true}},
		{MSG_SELECT_DISFIELD, {"select_disfield", "player:u8 count:u8 flag:u32", true}},
		{MSG_SORT_CARD, {"sort_card", "player:u8 cards:[u32]{code:u32 con:u8 loc:u32 seq:u32}", true}},
		{MSG_SELECT_UNSELECT_CARD, {"select_unselect_card",
			"player:u8 finishable:u8 cancelable:u8 min:u32 max:u32 select:[u32]{code:u32 at:loc} "
			"unselect:[u32]{code:u32 at:loc}", true}},
		{MSG_CONFIRM_DECKTOP, {"confirm_decktop", "player:u8 cards:[u32]{code:u32 con:u8 loc:u8 seq:u32}", false}},
		{MSG_CONFIRM_CARDS, {"confirm_cards", "player:u8 cards:[u32]{code:u32 con:u8 loc:u8 seq:u32}", false}},
		{MSG_SHUFFLE_DECK, {"shuffle_deck", "player:u8", false}},
		{MSG_SHUFFLE_HAND, {"shuffle_hand", "player:u8 cards:[u32]{code:u32}", false}},
		{MSG_REFRESH_DECK, {"refresh_deck", "player:u8", false}},
		{MSG_SWAP_GRAVE_DECK, {"swap_grave_deck", nullptr, false}},
		{MSG_SHUFFLE_SET_CARD, {"shuffle_set_card", nullptr, false}},
		{MSG_REVERSE_DECK, {"reverse_deck", "", false}},
		{MSG_DECK_TOP, {"deck_top", "player:u8 seq:u32 code:u32 pos:u32", false}},
		{MSG_SHUFFLE_EXTRA, {"shuffle_extra", "player:u8 cards:[u32]{code:u32}", false}},
		{MSG_NEW_TURN, {"new_turn", "player:u8", false}},
		{MSG_NEW_PHASE, {"new_phase", "phase:u16", false}},
		{MSG_CONFIRM_EXTRATOP, {"confirm_extratop", "player:u8 cards:[u32]{code:u32 con:u8 loc:u8 seq:u32}", false}},
		{MSG_MOVE, {"move", "code:u32 from:loc to:loc reason:u32", false}},
		{MSG_POS_CHANGE, {"pos_change", "code:u32 con:u8 loc:u8 seq:u8 prev:u8 pos:u8", false}},
		{MSG_SET, {"set", "code:u32 at:loc", false}},
		{MSG_SWAP, {"swap", "code1:u32 at1:loc code2:u32 at2:loc", false}},
		{MSG_FIELD_DISABLED, {"field_disabled", "flag:u32", false}},
		{MSG_SUMMONING, {"summoning", "code:u32 at:loc", false}},
		{MSG_SUMMONED, {"summoned", "", false}},
		{MSG_SPSUMMONING, {"spsummoning", "code:u32 at:loc", false}},
		{MSG_SPSUMMONED, {"spsummoned", "", false}},
		{MSG_FLIPSUMMONING, {"flipsummoning", "code:u32 at:loc", false}},
		{MSG_FLIPSUMMONED, {"flipsummoned", "", false}},
		{MSG_CHAINING, {"chaining", "code:u32 at:loc tcon:u8 tloc:u8 tseq:u32 desc:u64 count:u32", false}},
		{MSG_CHAINED, {"chained", "chain:u8", false}},
		{MSG_CHAIN_SOLVING, {"chain_solving", "chain:u8", false}},
		{MSG_CHAIN_SOLVED, {"chain_solved", "chain:u8", false}},
		{MSG_CHAIN_END, {"chain_end", "", false}},
		{MSG_CHAIN_NEGATED, {"chain_negated", "chain:u8", false}},
		{MSG_CHAIN_DISABLED, {"chain_disabled", "chain:u8", false}},
		{MSG_CARD_SELECTED, {"card_selected", "cards:[u32]{at:loc}", false}},
		{MSG_RANDOM_SELECTED, {"random_selected", "player:u8 cards:[u32]{at:loc}", false}},
		{MSG_BECOME_TARGET, {"become_target", "cards:[u32]{at:loc}", false}},
		{MSG_DRAW, {"draw", "player:u8 cards:[u32]{code:u32 pos:u32}", false}},
		{MSG_DAMAGE, {"damage", "player:u8 amount:u32", false}},
		{MSG_RECOVER, {"recover", "player:u8 amount:u32", false}},
		{MSG_EQUIP, {"equip", "card:loc target:loc", false}},
		{MSG_LPUPDATE, {"lp_update", "player:u8 lp:u32", false}},
		{MSG_UNEQUIP, {"unequip", "card:loc", false}},
		{MSG_CARD_TARGET, {"card_target", "card:loc target:loc", false}},
		{MSG_CANCEL_TARGET, {"cancel_target", "card:loc target:loc", false}},
		{MSG_PAY_LPCOST, {"pay_lpcost", "player:u8 amount:u32", false}},
		{MSG_ADD_COUNTER, {"add_counter", "ctype:u16 con:u8 loc:u8 seq:u8 count:u16", false}},
		{MSG_REMOVE_COUNTER, {"remove_counter", "ctype:u16 con:u8 loc:u8 seq:u8 count:u16", false}},
		{MSG_ATTACK, {"attack", "attacker:loc target:loc", false}},
		{MSG_BATTLE, {"battle", "attacker:loc aatk:u32 adef:u32 adestroyed:u8 target:loc datk:u32 ddef:u32 ddestroyed:u8", false}},
		{MSG_ATTACK_DISABLED, {"attack_disabled", "", false}},
		{MSG_DAMAGE_STEP_START, {"damage_step_start", "", false}},
		{MSG_DAMAGE_STEP_END, {"damage_step_end", "", false}},
		{MSG_MISSED_EFFECT, {"missed_effect", "at:loc code:u32", false}},
		{MSG_BE_CHAIN_TARGET, {"be_chain_target", nullptr, false}},
		{MSG_CREATE_RELATION, {"create_relation", nullptr, false}},
		{MSG_RELEASE_RELATION, {"release_relation", nullptr, false}},
		{MSG_TOSS_COIN, {"toss_coin", "player:u8 results:[u8]{v:u8}", false}},
		{MSG_TOSS_DICE, {"toss_dice", "player:u8 results:[u8]{v:u8}", false}},
		{MSG_ROCK_PAPER_SCISSORS, {"rock_paper_scissors", "player:u8", true}},
		{MSG_HAND_RES, {"hand_res", "res:u8", false}},
		{MSG_ANNOUNCE_RACE, {"announce_race", "player:u8 count:u8 available:u64", true}},
		{MSG_ANNOUNCE_ATTRIB, {"announce_attrib", "player:u8 count:u8 available:u32", true}},
		{MSG_ANNOUNCE_CARD, {"announce_card", "player:u8 opcodes:[u8]{op:u64}", true}},
		{MSG_ANNOUNCE_NUMBER, {"announce_number", "player:u8 options:[u8]{v:u64}", true}},
		{MSG_CARD_HINT, {"card_hint", "at:loc type:u8 value:u64", false}},
		{MSG_TAG_SWAP, {"tag_swap", nullptr, false}},
		{MSG_RELOAD_FIELD, {"reload_field", nullptr, false}},
		{MSG_PLAYER_HINT, {"player_hint", "player:u8 type:u8 value:u64", false}},
		{MSG_MATCH_KILL, {"match_kill", "code:u32", false}},
		{MSG_REMOVE_CARDS, {"remove_cards", "cards:[u32]{at:loc}", false}},
	};
	return m;
}

struct Reader {
	const uint8_t* b; size_t n, o = 0; bool over = false;
	template<typename T> T get() { T v{}; if(o + sizeof(T) <= n) std::memcpy(&v, b + o, sizeof(T)); else over = true; o += sizeof(T); return v; }
};

// Reads one schema (or a list's inner schema) from s[i..] up to a closing '}' or the end.
json read_schema(const char* s, size_t& i, Reader& r) {
	json obj = json::object();
	while(s[i] && s[i] != '}') {
		if(s[i] == ' ') { i++; continue; }
		size_t c = i; while(s[c] != ':') c++;
		std::string field(s + i, c - i);
		i = c + 1;
		if(s[i] == '[') {
			bool small = std::strncmp(s + i, "[u8]", 4) == 0;
			i += small ? 4 : 5;   // "[u8]" or "[u32]"
			uint32_t k = small ? r.get<uint8_t>() : r.get<uint32_t>();
			if(k > 4096) { r.over = true; k = 0; }
			i++;   // '{'
			size_t start = i;
			json arr = json::array();
			if(k == 0) { int depth = 1; while(s[i] && depth) { if(s[i] == '{') depth++; if(s[i] == '}') depth--; i++; } }
			for(uint32_t j = 0; j < k; j++) { i = start; arr.push_back(read_schema(s, i, r)); i++; }
			obj[field] = std::move(arr);
			continue;
		}
		size_t e = i; while(s[e] && s[e] != ' ' && s[e] != '}') e++;
		std::string t(s + i, e - i);
		i = e;
		if(t == "u8") obj[field] = r.get<uint8_t>();
		else if(t == "u16") obj[field] = r.get<uint16_t>();
		else if(t == "u32") obj[field] = r.get<uint32_t>();
		else if(t == "i32") obj[field] = r.get<int32_t>();
		else if(t == "u64") obj[field] = r.get<uint64_t>();
		else if(t == "loc") {
			json l; l["con"] = r.get<uint8_t>(); l["loc"] = r.get<uint8_t>(); l["seq"] = r.get<uint32_t>(); l["pos"] = r.get<uint32_t>();
			obj[field] = std::move(l);
		}
	}
	return obj;
}

std::string hex(const uint8_t* b, size_t n) {
	static const char* d = "0123456789abcdef";
	std::string s; s.reserve(n * 2);
	for(size_t i = 0; i < n; i++) { s += d[b[i] >> 4]; s += d[b[i] & 15]; }
	return s;
}

void put32(std::string& o, int32_t v) { o.append((const char*)&v, 4); }
}

const char* name(int type) { auto it = specs().find(type); return it == specs().end() ? "" : it->second.name; }
bool is_prompt(int type) { auto it = specs().find(type); return it != specs().end() && it->second.prompt; }

json decode(int type, const uint8_t* body, size_t len) {
	auto it = specs().find(type);
	json j;
	if(it == specs().end() || !it->second.schema) {
		j = json::object();
		j["raw"] = hex(body, len);
	} else {
		Reader r{body, len};
		size_t i = 0;
		j = read_schema(it->second.schema, i, r);
		if(r.over || r.o != len) { j["raw"] = hex(body, len); j["bad"] = true; }
	}
	j["t"] = it == specs().end() ? std::string("msg_") + std::to_string(type) : it->second.name;
	return j;
}

// Answers (client JSON) per prompt:
//   select_idlecmd   {"act":"summon|spsummon|repos|mset|sset|activate|battle|end|shuffle", "i":index in that list}
//   select_battlecmd {"act":"activate|attack|main2|end", "i":index}
//   select_effectyn / select_yesno {"yes":bool}
//   select_option / announce_number {"i":index}        select_chain {"i":index} or {"i":-1} (don't chain)
//   select_card / select_tribute / select_sum {"picks":[indexes]} or {"cancel":true}
//   select_unselect_card {"i":index into select then unselect} or {"done":true} (finish / cancel)
//   select_place / select_disfield {"places":[{"con":p,"loc":l,"seq":s}]}
//   select_position {"pos":POS_*}    select_counter {"counts":[per card]}    sort_card / sort_chain {"order":[...]} or {}
//   announce_race / announce_attrib {"value":mask}    announce_card {"code":card}    rock_paper_scissors {"choice":1..3}
bool encode_response(int type, const json& prompt, const json& a, std::string& out, std::string& err) {
	out.clear();
	auto geti = [&](const char* k, int64_t def) -> int64_t { return a.contains(k) && a[k].is_number() ? a[k].get<int64_t>() : def; };
	try {
		switch(type) {
		case MSG_SELECT_IDLECMD: {
			static const char* acts[] = {"summon", "spsummon", "repos", "mset", "sset", "activate", "battle", "end", "shuffle"};
			std::string act = a.value("act", "");
			for(int t = 0; t < 9; t++) if(act == acts[t]) { put32(out, (int32_t)((geti("i", 0) << 16) | t)); return true; }
			err = "unknown idle action"; return false;
		}
		case MSG_SELECT_BATTLECMD: {
			static const char* acts[] = {"activate", "attack", "main2", "end"};
			std::string act = a.value("act", "");
			for(int t = 0; t < 4; t++) if(act == acts[t]) { put32(out, (int32_t)((geti("i", 0) << 16) | t)); return true; }
			err = "unknown battle action"; return false;
		}
		case MSG_SELECT_EFFECTYN: case MSG_SELECT_YESNO: put32(out, a.value("yes", false) ? 1 : 0); return true;
		case MSG_SELECT_OPTION: case MSG_ANNOUNCE_NUMBER: case MSG_SELECT_CHAIN: put32(out, (int32_t)geti("i", -1)); return true;
		case MSG_SELECT_CARD: case MSG_SELECT_TRIBUTE: case MSG_SELECT_SUM: {
			if(a.value("cancel", false)) { put32(out, -1); return true; }
			if(!a.contains("picks") || !a["picks"].is_array()) { err = "picks missing"; return false; }
			put32(out, 0); put32(out, (int32_t)a["picks"].size());
			for(auto& p : a["picks"]) put32(out, p.get<int32_t>());
			return true;
		}
		case MSG_SELECT_UNSELECT_CARD:
			if(a.value("done", false)) { put32(out, -1); return true; }
			put32(out, 1); put32(out, (int32_t)geti("i", 0)); return true;
		case MSG_SELECT_PLACE: case MSG_SELECT_DISFIELD: {
			if(!a.contains("places") || !a["places"].is_array()) { err = "places missing"; return false; }
			for(auto& p : a["places"]) { out += (char)p.value("con", 0); out += (char)p.value("loc", 0); out += (char)p.value("seq", 0); }
			return true;
		}
		case MSG_SELECT_POSITION: put32(out, (int32_t)geti("pos", 1)); return true;
		case MSG_SELECT_COUNTER: {
			for(auto& c : a.value("counts", json::array())) { int16_t v = c.get<int16_t>(); out.append((const char*)&v, 2); }
			return true;
		}
		case MSG_SORT_CARD: case MSG_SORT_CHAIN: {
			if(!a.contains("order") || !a["order"].is_array()) { out += (char)-1; return true; }
			for(auto& v : a["order"]) out += (char)v.get<int>();
			return true;
		}
		case MSG_ANNOUNCE_RACE: { uint64_t v = a.value("value", (uint64_t)0); out.append((const char*)&v, 8); return true; }
		case MSG_ANNOUNCE_ATTRIB: { uint32_t v = a.value("value", (uint32_t)0); out.append((const char*)&v, 4); return true; }
		case MSG_ANNOUNCE_CARD: put32(out, (int32_t)geti("code", 0)); return true;
		case MSG_ROCK_PAPER_SCISSORS: put32(out, (int32_t)geti("choice", 1)); return true;
		}
	} catch(const std::exception& e) { err = e.what(); return false; }
	(void)prompt;
	err = "not a prompt";
	return false;
}

}
