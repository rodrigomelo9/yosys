#include "kernel/yosys.h"
#include "kernel/celltypes.h"
#include "kernel/ff.h"
#include "kernel/ffinit.h"
#include <variant>
#include <charconv>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct EnableLogic {
	SigBit bit;
	bool pol;
};

enum SliceIndices {
	RtlilSlice,
	HdlSlice,
};

struct Slice {
	SliceIndices indices;
	int first;
	int last;

	Slice(SliceIndices indices, const std::string &slice) :
		indices(indices)
	{
		if (slice.empty())
			syntax_error(slice);
		auto sep = slice.find(':');
		const char *first_begin, *first_end, *last_begin, *last_end;
		if (sep == std::string::npos) {
			first_begin = last_begin = slice.c_str();
			first_end = last_end = slice.c_str() + slice.length();
		} else {
			first_begin = slice.c_str();
			first_end = first_begin + sep;

			last_begin = first_end + 1;
			last_end = slice.c_str() + slice.length();
		}
		first = parse_index(first_begin, first_end, slice);
		last = parse_index(last_begin, last_end, slice);
	}

	static int parse_index(const char *begin, const char *end, const std::string &slice) {
		int value;
		auto result = std::from_chars(begin, end, value, 10);
        if (result.ptr != end || result.ptr == begin)
			syntax_error(slice);
		return value;
	}

	static void syntax_error(const std::string &slice) {
		log_cmd_error("Invalid slice '%s', expected '<first>:<last>' or '<single>'", slice.c_str());
	}

	std::string to_string() const {
		const char *option = indices == RtlilSlice ? "-rawslice" : "-slice";
		if (first == last)
			return stringf("%s %d", option, first);
		else
			return stringf("%s %d:%d", option, first, last);
	}

	int wire_offset(RTLIL::Wire *wire, int index) const {
		int rtl_offset = indices == RtlilSlice ? index : wire->from_hdl_index(index);
		if (rtl_offset < 0 || rtl_offset >= wire->width) {
			log_error("Slice %s is out of bounds for wire %s in module %s", to_string().c_str(), log_id(wire), log_id(wire->module));
		}
		return rtl_offset;
	}

	std::pair<int, int> wire_range(RTLIL::Wire *wire) const {
		int rtl_first = wire_offset(wire, first);
		int rtl_last = wire_offset(wire, last);
		if (rtl_first > rtl_last)
			std::swap(rtl_first, rtl_last);
		return {rtl_first, rtl_last + 1};
	}
};

void emit_mux_anyseq(Module* mod, const SigSpec& mux_input, const SigSpec& mux_output, EnableLogic enable) {
	auto anyseq = mod->Anyseq(NEW_ID, mux_input.size());
	if (enable.bit == (enable.pol ? State::S1 : State::S0)) {
		mod->connect(mux_output, anyseq);
	}
	SigSpec mux_a, mux_b;
	if (enable.pol) {
		mux_a = mux_input;
		mux_b = anyseq;
	} else {
		mux_a = anyseq;
		mux_b = mux_input;
	}
	(void)mod->addMux(NEW_ID,
		mux_a,
		mux_b,
		enable.bit,
		mux_output);
}

bool abstract_state_port(FfData& ff, SigSpec& port_sig, std::set<int> offsets, EnableLogic enable) {
	Wire* abstracted = ff.module->addWire(NEW_ID, offsets.size());
	SigSpec mux_input;
	int abstracted_idx = 0;
	for (int d_idx = 0; d_idx < ff.width; d_idx++) {
		if (offsets.count(d_idx)) {
			mux_input.append(port_sig[d_idx]);
			port_sig[d_idx].wire = abstracted;
			port_sig[d_idx].offset = abstracted_idx;
			log_assert(abstracted_idx < abstracted->width);
			abstracted_idx++;
		}
	}
	emit_mux_anyseq(ff.module, mux_input, abstracted, enable);
	(void)ff.emit();
	return true;
}

using SelReason=std::variant<Wire*, Cell*>;

dict<SigBit, std::vector<SelReason>> gather_selected_reps(Module* mod, const std::vector<Slice> &slices, SigMap& sigmap) {
	dict<SigBit, std::vector<SelReason>> selected_reps;

	if (slices.empty()) {
		// Collect reps for all wire bits of selected wires
		for (auto wire : mod->selected_wires())
			for (auto bit : sigmap(wire))
				selected_reps.insert(bit).first->second.push_back(wire);

		// Collect reps for all output wire bits of selected cells
		for (auto cell : mod->selected_cells())
			for (auto conn : cell->connections())
				if (cell->output(conn.first))
					for (auto bit : conn.second.bits())
						selected_reps.insert(sigmap(bit)).first->second.push_back(cell);
	} else {
		if (mod->selected_wires().size() != 1 || !mod->selected_cells().empty())
			log_error("Slices are only supported for single-wire selections\n");

		auto wire = mod->selected_wires()[0];

		for (auto slice : slices) {
			auto [begin, end] = slice.wire_range(wire);
			for (int i = begin; i < end; i++) {
				selected_reps.insert(sigmap(SigBit(wire, i))).first->second.push_back(wire);
			}
		}

	}
	return selected_reps;
}

void explain_selections(const std::vector<SelReason>& reasons) {
	for (std::variant<Wire*, Cell*> reason : reasons) {
		if (Cell** cell_reason = std::get_if<Cell*>(&reason))
			log_debug("\tcell %s\n", (*cell_reason)->name.c_str());
		else if (Wire** wire_reason = std::get_if<Wire*>(&reason))
			log_debug("\twire %s\n", (*wire_reason)->name.c_str());
		else
			log_assert(false && "insane reason variant\n");
	}
}

unsigned int abstract_state(Module* mod, EnableLogic enable, const std::vector<Slice> &slices) {
	CellTypes ct;
	ct.setup_internals_ff();
	SigMap sigmap(mod);
	dict<SigBit, std::vector<SelReason>> selected_reps = gather_selected_reps(mod, slices, sigmap);

	unsigned int changed = 0;
	std::vector<FfData> ffs;
	// Abstract flop inputs if they're driving a selected output rep
	for (auto cell : mod->cells()) {
		if (!ct.cell_types.count(cell->type))
			continue;
		FfData ff(nullptr, cell);
		if (ff.has_sr)
			log_cmd_error("SR not supported\n");
		ffs.push_back(ff);
	}
	for (auto ff : ffs) {
		// A bit inefficient
		std::set<int> offsets_to_abstract;
		for (int i = 0; i < GetSize(ff.sig_q); i++) {
			SigBit bit = ff.sig_q[i];
			if (selected_reps.count(sigmap(bit))) {
				log_debug("Abstracting state for bit %s due to selections:\n", log_signal(bit));
				explain_selections(selected_reps.at(sigmap(bit)));
				offsets_to_abstract.insert(i);
			}
		}

		if (offsets_to_abstract.empty())
			continue;

		// Normalize to simpler FF
		ff.unmap_ce();
		ff.unmap_srst();
		if (ff.has_arst)
			ff.arst_to_aload();

		if (ff.has_aload)
			changed += abstract_state_port(ff, ff.sig_ad, offsets_to_abstract, enable);
		changed += abstract_state_port(ff, ff.sig_d, offsets_to_abstract, enable);
	}
	return changed;
}

bool abstract_value_port(Module* mod, Cell* cell, std::set<int> offsets, IdString port_name, EnableLogic enable) {
	Wire* to_abstract = mod->addWire(NEW_ID, offsets.size());
	SigSpec mux_input;
	SigSpec mux_output;
	const SigSpec& old_port = cell->getPort(port_name);
	SigSpec new_port = old_port;
	int to_abstract_idx = 0;
	for (int port_idx = 0; port_idx < old_port.size(); port_idx++) {
		if (offsets.count(port_idx)) {
			mux_output.append(old_port[port_idx]);
			SigBit in_bit {to_abstract, to_abstract_idx};
			new_port.replace(port_idx, in_bit);
			mux_input.append(in_bit);
			log_assert(to_abstract_idx < to_abstract->width);
			to_abstract_idx++;
		}
	}
	cell->setPort(port_name, new_port);
	emit_mux_anyseq(mod, mux_input, mux_output, enable);
	return true;
}

unsigned int abstract_value(Module* mod, EnableLogic enable, const std::vector<Slice> &slices) {
	SigMap sigmap(mod);
	dict<SigBit, std::vector<SelReason>> selected_reps = gather_selected_reps(mod, slices, sigmap);
	unsigned int changed = 0;
	std::vector<Cell*> cells_snapshot = mod->cells();
	for (auto cell : cells_snapshot) {
		for (auto conn : cell->connections())
			if (cell->output(conn.first)) {
				std::set<int> offsets_to_abstract;
				for (int i = 0; i < conn.second.size(); i++) {
					if (selected_reps.count(sigmap(conn.second[i]))) {
						log_debug("Abstracting value for bit %s due to selections:\n", log_signal(conn.second[i]));
						explain_selections(selected_reps.at(sigmap(conn.second[i])));
						offsets_to_abstract.insert(i);
					}
				}
				if (offsets_to_abstract.empty())
					continue;

				changed += abstract_value_port(mod, cell, offsets_to_abstract, conn.first, enable);
			}
	}
	return changed;
}

unsigned int abstract_init(Module* mod, const std::vector<Slice> &slices) {
	unsigned int changed = 0;
	FfInitVals initvals;
	SigMap sigmap(mod);
	dict<SigBit, std::vector<SelReason>> selected_reps = gather_selected_reps(mod, slices, sigmap);
	initvals.set(&sigmap, mod);
	for (auto bit : selected_reps) {
		log_debug("Removing init bit on %s due to selections:\n", log_signal(bit.first));
		explain_selections(bit.second);
		initvals.remove_init(bit.first);
		changed++;
	}
	return changed;
}

struct AbstractPass : public Pass {
	AbstractPass() : Pass("abstract", "extract clock gating out of flip flops") { }
	void help() override {
		// TODO
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
	}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override {
		log_header(design, "Executing ABSTRACT pass.\n");

		size_t argidx;
		enum Mode {
			None,
			State,
			Initial,
			Value,
		};
		Mode mode = Mode::None;
		enum Enable {
			Always = -1,
			ActiveLow = false, // ensuring we can use bool(enable)
			ActiveHigh = true,
		};
		Enable enable = Enable::Always;
		std::string enable_name;
		std::vector<Slice> slices;
		for (argidx = 1; argidx < args.size(); argidx++)
		{
			std::string arg = args[argidx];
			if (arg == "-state") {
				mode = State;
				continue;
			}
			if (arg == "-init") {
				mode = Initial;
				continue;
			}
			if (arg == "-value") {
				mode = Value;
				continue;
			}
			if (arg == "-enable" && argidx + 1 < args.size()) {
				if (enable != Enable::Always)
					log_cmd_error("Multiple enable condition are not supported\n");
				enable_name = args[++argidx];
				enable = Enable::ActiveHigh;
				continue;
			}
			if (arg == "-enablen" && argidx + 1 < args.size()) {
				if (enable != Enable::Always)
					log_cmd_error("Multiple enable condition are not supported\n");
				enable_name = args[++argidx];
				enable = Enable::ActiveLow;
				continue;
			}
			if (arg == "-slice" && argidx + 1 < args.size()) {
				slices.emplace_back(SliceIndices::HdlSlice, args[++argidx]);
				continue;
			}
			if (arg == "-rtlilslice" && argidx + 1 < args.size()) {
				slices.emplace_back(SliceIndices::RtlilSlice, args[++argidx]);
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);

		if (enable != Enable::Always) {
			if (mode == Mode::Initial)
				log_cmd_error("Conditional initial value abstraction is not supported\n");

			if (enable_name.empty())
				log_cmd_error("Unspecified enable wire\n");
		}

		unsigned int changed = 0;
		if ((mode == State) || (mode == Value)) {
			for (auto mod : design->selected_modules()) {
				EnableLogic enable_logic = { State::S1, true };
				if (enable != Enable::Always) {
					Wire *enable_wire = mod->wire("\\" + enable_name);
					if (!enable_wire)
						log_cmd_error("Enable wire %s not found in module %s\n", enable_name.c_str(), mod->name.c_str());
					if (GetSize(enable_wire) != 1)
						log_cmd_error("Enable wire %s must have width 1 but has width %d in module %s\n",
								enable_name.c_str(), GetSize(enable_wire), mod->name.c_str());
					enable_logic = { enable_wire, enable == Enable::ActiveHigh };
				}
				if (mode == State)
					changed += abstract_state(mod, enable_logic, slices);
				else
					changed += abstract_value(mod, enable_logic, slices);
			}
			if (mode == State)
				log("Abstracted %d stateful cells.\n", changed);
			else
				log("Abstracted %d driver ports.\n", changed);
		} else if (mode == Initial) {
			for (auto mod : design->selected_modules()) {
				changed += abstract_init(mod, slices);
			}
			log("Abstracted %d init bits.\n", changed);
		} else {
			log_cmd_error("No mode selected, see help message\n");
		}
	}
} AbstractPass;

PRIVATE_NAMESPACE_END
