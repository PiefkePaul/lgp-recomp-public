// Post-analysis script: collects code statistics for the recompiler ADRs (ADR-0004/0005).
// Writes only counts and ISA mnemonic names as JSON, never instruction bytes or listings.
//
// Collected:
//   - functions (count, thunks, FDE coverage, size distribution)
//   - instruction mix (integer, load/store, FP scalar, NEON, atomics, system, ...)
//   - indirect branches (BR/BLR/RET) classified by how the target is produced
//   - jump tables recovered by Ghidra's switch analysis (count, cases, entry width)
//   - address-taken functions (possible BLR targets) and pointer tables in data
//   - call counts of selected imports (setjmp/longjmp, exception runtime, ...)
//
// Usage (headless): -postScript LgpCodeStats.java <module.meta.json> <out.json>
// @category LGP

import java.io.FileReader;
import java.io.FileWriter;
import java.io.Reader;
import java.io.Writer;
import java.util.*;
import java.util.regex.Pattern;

import com.google.gson.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.*;

public class LgpCodeStats extends GhidraScript {

	private static final Pattern NEON_ARRANGEMENT =
		Pattern.compile("\\.(8b|16b|4h|8h|2s|4s|1d|2d|1q|b|h|s|d)(\\[|\\b)", Pattern.CASE_INSENSITIVE);
	private static final Pattern VREG = Pattern.compile("^v\\d+", Pattern.CASE_INSENSITIVE);
	private static final Pattern FPREG = Pattern.compile("^[qdshb]\\d+$", Pattern.CASE_INSENSITIVE);

	private static final Set<String> TRACKED_IMPORTS = new HashSet<>(Arrays.asList(
		"setjmp", "longjmp", "_setjmp", "_longjmp", "sigsetjmp", "siglongjmp",
		"__cxa_throw", "__cxa_rethrow", "__cxa_begin_catch", "__cxa_end_catch",
		"__cxa_allocate_exception", "_Unwind_Resume", "_Unwind_RaiseException",
		"__gxx_personality_v0", "__cxa_pure_virtual", "__cxa_guard_acquire",
		"__cxa_atexit", "abort"));

	private Memory mem;
	private Listing listing;
	private FunctionManager fm;
	private ReferenceManager refs;
	private AddressSet textSet;

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 2) {
			throw new IllegalArgumentException("usage: LgpCodeStats.java <meta.json> <out.json>");
		}
		JsonObject meta;
		try (Reader r = new FileReader(args[0])) {
			meta = JsonParser.parseReader(r).getAsJsonObject();
		}
		mem = currentProgram.getMemory();
		listing = currentProgram.getListing();
		fm = currentProgram.getFunctionManager();
		refs = currentProgram.getReferenceManager();
		Address base = currentProgram.getImageBase();

		textSet = new AddressSet();
		long textBytes = 0;
		for (MemoryBlock b : mem.getBlocks()) {
			if (b.isExecute() && b.isInitialized() && !b.isExternalBlock()) {
				textSet.add(b.getStart(), b.getEnd());
				textBytes += b.getSize();
			}
		}

		Set<Long> fdeStarts = new HashSet<>();
		List<Long> fdeSizes = new ArrayList<>();
		for (JsonElement e : meta.getAsJsonArray("fdes")) {
			JsonArray a = e.getAsJsonArray();
			fdeStarts.add(base.add(a.get(0).getAsLong()).getOffset());
			fdeSizes.add(a.get(1).getAsLong());
		}

		JsonObject out = new JsonObject();
		out.addProperty("module", meta.get("module").getAsString());
		out.addProperty("module_id", meta.get("module_id").getAsString());
		out.addProperty("ghidra_version", getGhidraVersion());
		out.addProperty("text_bytes", textBytes);
		out.add("fde", fdeStats(meta, fdeSizes));
		out.add("functions", functionStats(fdeStarts));
		Set<Address> taken = new HashSet<>();
		out.add("instructions", instructionStats(fdeStarts, taken));
		out.add("data_pointers", dataPointerStats(taken));
		out.add("imports", importStats());

		try (Writer w = new FileWriter(args[1])) {
			new GsonBuilder().setPrettyPrinting().disableHtmlEscaping().create().toJson(out, w);
		}
		println("LgpCodeStats: wrote " + args[1]);
	}

	// ---------------------------------------------------------------- helpers

	private static JsonObject distribution(List<Long> values, long[] bucketLimits) {
		JsonObject o = new JsonObject();
		List<Long> v = new ArrayList<>(values);
		Collections.sort(v);
		o.addProperty("count", v.size());
		if (v.isEmpty()) {
			return o;
		}
		long sum = 0;
		for (long x : v) {
			sum += x;
		}
		o.addProperty("sum", sum);
		o.addProperty("min", v.get(0));
		o.addProperty("p50", v.get(v.size() / 2));
		o.addProperty("p90", v.get((int) (v.size() * 0.9)));
		o.addProperty("p99", v.get((int) (v.size() * 0.99)));
		o.addProperty("max", v.get(v.size() - 1));
		JsonObject buckets = new JsonObject();
		long prev = 0;
		for (long lim : bucketLimits) {
			long n = 0;
			for (long x : v) {
				if (x > prev && x <= lim) {
					n++;
				}
			}
			buckets.addProperty("<=" + lim, n);
			prev = lim;
		}
		long n = 0;
		for (long x : v) {
			if (x > prev) {
				n++;
			}
		}
		buckets.addProperty(">" + prev, n);
		o.add("buckets", buckets);
		return o;
	}

	private static final long[] SIZE_BUCKETS = { 16, 64, 256, 1024, 4096, 16384, 65536 };

	private static void inc(Map<String, Long> m, String k) {
		m.merge(k, 1L, Long::sum);
	}

	private static void inc(Map<String, Long> m, String k, long n) {
		m.merge(k, n, Long::sum);
	}

	private static JsonObject toJson(Map<String, Long> m) {
		JsonObject o = new JsonObject();
		m.entrySet()
				.stream()
				.sorted((a, b) -> Long.compare(b.getValue(), a.getValue()))
				.forEach(e -> o.addProperty(e.getKey(), e.getValue()));
		return o;
	}

	private static Register baseReg(Object o) {
		if (o instanceof Register) {
			Register r = (Register) o;
			Register b = r.getBaseRegister();
			return b != null ? b : r;
		}
		return null;
	}

	// ---------------------------------------------------------------- FDEs

	private JsonObject fdeStats(JsonObject meta, List<Long> fdeSizes) {
		JsonObject o = new JsonObject();
		o.addProperty("count", meta.get("fde_count").getAsLong());
		o.addProperty("with_lsda", meta.get("fde_with_lsda").getAsLong());
		o.addProperty("with_personality", meta.get("fde_with_personality").getAsLong());
		o.add("size_bytes", distribution(fdeSizes, SIZE_BUCKETS));
		return o;
	}

	// ---------------------------------------------------------------- functions

	private JsonObject functionStats(Set<Long> fdeStarts) throws Exception {
		JsonObject o = new JsonObject();
		long total = 0, thunks = 0, atFde = 0, noFde = 0, noFdeThunk = 0, noReturn = 0;
		List<Long> sizes = new ArrayList<>();
		Set<Long> seen = new HashSet<>();
		for (Function f : fm.getFunctions(true)) {
			monitor.checkCancelled();
			if (f.isExternal()) {
				continue;
			}
			total++;
			long entry = f.getEntryPoint().getOffset();
			seen.add(entry);
			if (f.isThunk()) {
				thunks++;
			}
			if (f.hasNoReturn()) {
				noReturn++;
			}
			if (fdeStarts.contains(entry)) {
				atFde++;
			}
			else {
				noFde++;
				if (f.isThunk()) {
					noFdeThunk++;
				}
			}
			if (!f.isThunk()) {
				sizes.add(f.getBody().getNumAddresses());
			}
		}
		long fdeWithoutFunction = 0;
		for (long s : fdeStarts) {
			if (!seen.contains(s)) {
				fdeWithoutFunction++;
			}
		}
		o.addProperty("total", total);
		o.addProperty("thunks", thunks);
		o.addProperty("no_return", noReturn);
		o.addProperty("at_fde_start", atFde);
		o.addProperty("without_fde", noFde);
		o.addProperty("without_fde_thunks", noFdeThunk);
		o.addProperty("fde_without_function", fdeWithoutFunction);
		o.add("body_bytes_non_thunk", distribution(sizes, SIZE_BUCKETS));
		return o;
	}

	// ---------------------------------------------------------------- instructions

	private static boolean isLoadStore(String m) {
		return m.startsWith("ld") || m.startsWith("st") || m.startsWith("prfm") || m.startsWith("prfum");
	}

	private static boolean isExclusive(String m) {
		return m.matches("(ld|st)(a|l)?x(r|p)[bh]?") || m.equals("clrex");
	}

	private static boolean isLse(String m) {
		return m.matches("(cas|casp|swp|ld(add|clr|eor|set|smax|smin|umax|umin)|st(add|clr|eor|set|smax|smin|umax|umin))(a|l|al)?[bh]?");
	}

	private static boolean isAcqRel(String m) {
		return m.matches("(ldar|stlr|ldapr|ldlar|stllr)[bh]?");
	}

	private String operandText(Instruction in, int i) {
		String s = in.getDefaultOperandRepresentation(i);
		return s == null ? "" : s;
	}

	/** Returns: 0 = GPR only, 1 = FP/SIMD scalar register involved, 2 = vector arrangement. */
	private int simdKind(Instruction in) {
		int kind = 0;
		for (int i = 0; i < in.getNumOperands(); i++) {
			String t = operandText(in, i);
			if (VREG.matcher(t).find() && NEON_ARRANGEMENT.matcher(t).find()) {
				return 2;
			}
			if (t.startsWith("{")) { // ld1/st1 register lists
				return 2;
			}
			for (Object o : in.getOpObjects(i)) {
				if (o instanceof Register) {
					String n = ((Register) o).getName();
					if (FPREG.matcher(n).matches()) {
						kind = 1;
					}
				}
			}
		}
		return kind;
	}

	private String classify(Instruction in, String m) {
		if (m.equals("br") || m.equals("blr") || m.equals("ret") || m.startsWith("bra") ||
			m.startsWith("blra") || m.startsWith("reta")) {
			return "branch_indirect";
		}
		if (m.equals("b") || m.equals("bl") || m.startsWith("b.") || m.equals("cbz") ||
			m.equals("cbnz") || m.equals("tbz") || m.equals("tbnz")) {
			return "branch_direct";
		}
		if (isExclusive(m)) {
			return "atomic_exclusive";
		}
		if (isLse(m)) {
			return "atomic_lse";
		}
		if (isAcqRel(m)) {
			return "load_acquire_store_release";
		}
		if (m.equals("svc") || m.equals("mrs") || m.equals("msr") || m.equals("sys") ||
			m.equals("sysl") || m.equals("dc") || m.equals("ic") || m.equals("dmb") ||
			m.equals("dsb") || m.equals("isb") || m.equals("hint") || m.equals("nop") ||
			m.equals("yield") || m.equals("wfe") || m.equals("wfi") || m.equals("sev") ||
			m.equals("sevl") || m.equals("brk") || m.equals("hlt") || m.equals("udf") ||
			m.startsWith("pac") || m.startsWith("aut") || m.equals("xpaclri")) {
			return "system";
		}
		if (m.startsWith("aes") || m.startsWith("sha1") || m.startsWith("sha256") ||
			m.startsWith("sha512") || m.startsWith("pmull") || m.startsWith("crc32")) {
			return "crypto_crc";
		}
		int k = simdKind(in);
		if (isLoadStore(m)) {
			return k == 0 ? "load_store_gpr" : "load_store_fpsimd";
		}
		if (k == 2) {
			return "neon_vector";
		}
		if (k == 1 || m.startsWith("f") || m.equals("scvtf") || m.equals("ucvtf")) {
			return "fp_scalar";
		}
		return "int_alu";
	}

	/** Walks back inside the basic block to find the instruction that last wrote reg. */
	private Instruction definer(Instruction from, Register reg, int maxSteps) {
		Instruction cur = from;
		for (int i = 0; i < maxSteps; i++) {
			if (refs.hasReferencesTo(cur.getAddress()) && cur != from) {
				// cur is a branch target; earlier instructions may not dominate
				// (only flow references count; data refs are rare inside code)
				boolean flowIn = false;
				for (Reference r : refs.getReferencesTo(cur.getAddress())) {
					if (r.getReferenceType().isFlow()) {
						flowIn = true;
						break;
					}
				}
				if (flowIn) {
					return null;
				}
			}
			Instruction prev = cur.getPrevious();
			if (prev == null || !prev.getAddress().add(prev.getLength()).equals(cur.getAddress())) {
				return null;
			}
			FlowType ft = prev.getFlowType();
			if (ft.isJump() || ft.isTerminal() || ft.isCall()) {
				return null;
			}
			for (Object o : prev.getResultObjects()) {
				Register r = baseReg(o);
				if (r != null && r.equals(reg)) {
					return prev;
				}
			}
			cur = prev;
		}
		return null;
	}

	private Register targetReg(Instruction in) {
		for (Object o : in.getOpObjects(0)) {
			Register r = baseReg(o);
			if (r != null) {
				return r;
			}
		}
		return null;
	}

	/** First register operand inside a memory operand of a load: ldr xT, [xB, ...]. */
	private Register loadBase(Instruction ld) {
		if (ld.getNumOperands() < 2) {
			return null;
		}
		for (Object o : ld.getOpObjects(1)) {
			Register r = baseReg(o);
			if (r != null) {
				return r;
			}
		}
		return null;
	}

	private String blrPattern(Instruction in) {
		Register t = targetReg(in);
		if (t == null) {
			return "unknown_operand";
		}
		Instruction d = definer(in, t, 16);
		if (d == null) {
			return "defined_outside_block";
		}
		String dm = d.getMnemonicString().toLowerCase();
		if (dm.startsWith("ldr") || dm.startsWith("ldur")) {
			Register b = loadBase(d);
			if (b == null) {
				return "load_literal";
			}
			if (b.getName().equalsIgnoreCase("sp")) {
				return "load_from_stack";
			}
			Instruction d2 = definer(d, b, 16);
			if (d2 != null) {
				String m2 = d2.getMnemonicString().toLowerCase();
				if (m2.startsWith("ldr") || m2.startsWith("ldur")) {
					return "load_of_load_vtable_like";
				}
				if (m2.equals("adrp") || m2.equals("add") || m2.equals("adr")) {
					return "load_from_global";
				}
			}
			return "load_from_object";
		}
		if (dm.equals("adrp") || dm.equals("add") || dm.equals("adr")) {
			return "address_constant";
		}
		if (dm.equals("mov") || dm.equals("orr")) {
			return "register_copy";
		}
		return "computed_" + dm;
	}

	private String brPattern(Instruction in) {
		Register t = targetReg(in);
		if (t == null) {
			return "unknown_operand";
		}
		Instruction d = definer(in, t, 16);
		if (d == null) {
			return "defined_outside_block";
		}
		String dm = d.getMnemonicString().toLowerCase();
		if (dm.equals("add") || dm.equals("sub")) {
			// clang jump-table idiom: ldr{b,h,sw} idx -> add base, base, idx, lsl #2 -> br
			for (int i = 1; i < d.getNumOperands(); i++) {
				for (Object o : d.getOpObjects(i)) {
					Register r = baseReg(o);
					if (r == null) {
						continue;
					}
					Instruction ld = definer(d, r, 16);
					if (ld != null) {
						String lm = ld.getMnemonicString().toLowerCase();
						if (lm.startsWith("ldr")) {
							return "jumptable_idiom_" + lm;
						}
					}
				}
			}
			return "computed_add";
		}
		if (dm.startsWith("ldr") || dm.startsWith("ldur")) {
			return "loaded_pointer";
		}
		return "computed_" + dm;
	}

	private JsonObject instructionStats(Set<Long> fdeStarts, Set<Address> taken) throws Exception {
		Map<String, Long> classes = new TreeMap<>();
		Map<String, Long> mnemonics = new HashMap<>();
		Map<String, Long> sysregs = new HashMap<>();
		Map<String, Long> svcs = new HashMap<>();
		Map<String, Long> br = new TreeMap<>();
		Map<String, Long> blr = new TreeMap<>();
		Map<String, Long> ret = new TreeMap<>();
		Map<String, Long> jt = new TreeMap<>();
		List<Long> jtCases = new ArrayList<>();
		long total = 0, directCalls = 0, callToNonEntry = 0, tailCalls = 0, callToNoFde = 0;
		Set<Address> blTargetsNoFde = new HashSet<>();

		InstructionIterator it = listing.getInstructions(textSet, true);
		while (it.hasNext()) {
			Instruction in = it.next();
			if ((++total & 0xFFFF) == 0) {
				monitor.checkCancelled();
			}
			String m = in.getMnemonicString().toLowerCase();
			if (m.startsWith("_")) {
				m = m.substring(1); // Ghidra marks some forms with a leading underscore
			}
			inc(mnemonics, m);
			String cls = classify(in, m);
			inc(classes, cls);

			if (m.equals("mrs")) {
				inc(sysregs, "mrs " + operandText(in, 1).toLowerCase());
			}
			else if (m.equals("msr")) {
				inc(sysregs, "msr " + operandText(in, 0).toLowerCase());
			}
			else if (m.equals("svc")) {
				inc(svcs, operandText(in, 0).toLowerCase());
			}

			if (m.equals("bl")) {
				directCalls++;
				for (Reference r : in.getReferencesFrom()) {
					if (r.getReferenceType().isCall()) {
						Address to = r.getToAddress();
						Function f = fm.getFunctionAt(to);
						if (f == null) {
							callToNonEntry++;
						}
						else if (!fdeStarts.contains(to.getOffset()) && !f.isThunk()) {
							callToNoFde++;
							blTargetsNoFde.add(to);
						}
					}
				}
			}
			else if (m.equals("b")) {
				Function here = fm.getFunctionContaining(in.getAddress());
				for (Reference r : in.getReferencesFrom()) {
					if (!r.getReferenceType().isFlow()) {
						continue;
					}
					Function f = fm.getFunctionAt(r.getToAddress());
					if (f != null && f != here) {
						tailCalls++;
					}
				}
			}
			else if (m.equals("blr") || m.startsWith("blra")) {
				boolean resolved = false;
				for (Reference r : in.getReferencesFrom()) {
					if (r.getReferenceType().isCall() && r.getToAddress().isMemoryAddress()) {
						resolved = true;
					}
				}
				inc(blr, "total");
				inc(blr, resolved ? "resolved_by_ghidra" : "unresolved");
				inc(blr, "pattern_" + blrPattern(in));
			}
			else if (m.equals("br") || m.startsWith("bra")) {
				inc(br, "total");
				Function here = fm.getFunctionContaining(in.getAddress());
				int computedTargets = 0;
				boolean toFunction = false;
				for (Reference r : in.getReferencesFrom()) {
					RefType rt = r.getReferenceType();
					if (!rt.isFlow()) {
						continue;
					}
					Function f = fm.getFunctionAt(r.getToAddress());
					if (f != null && f != here) {
						toFunction = true;
					}
					else if (rt.isComputed() || rt.isJump()) {
						computedTargets++;
					}
				}
				String pat = brPattern(in);
				if (here != null && here.isThunk()) {
					inc(br, "in_thunk_plt");
				}
				else if (computedTargets > 1) {
					inc(br, "jumptable_recovered");
					inc(jt, "recovered");
					inc(jt, "cases_total", computedTargets);
					jtCases.add((long) computedTargets);
					inc(jt, "idiom_" + pat);
				}
				else if (toFunction) {
					inc(br, "tailcall_resolved");
				}
				else {
					inc(br, "unresolved");
					inc(br, "unresolved_pattern_" + pat);
				}
			}
			else if (m.equals("ret") || m.startsWith("reta")) {
				Register t = targetReg(in);
				inc(ret, t == null || t.getName().equalsIgnoreCase("x30") ? "via_x30" : "via_other_reg");
			}

			// code references that take a function's address (adrp/add, adr, ldr literal)
			if (!in.getFlowType().isCall() && !in.getFlowType().isJump()) {
				for (Reference r : in.getReferencesFrom()) {
					if (r.getReferenceType().isFlow()) {
						continue;
					}
					Function f = fm.getFunctionAt(r.getToAddress());
					if (f != null && !f.isExternal()) {
						taken.add(f.getEntryPoint());
					}
				}
			}
		}

		JsonObject o = new JsonObject();
		o.addProperty("total", total);
		o.add("classes", toJson(classes));
		o.add("mnemonics", toJson(mnemonics));
		o.add("sysregs", toJson(sysregs));
		o.add("svc_immediates", toJson(svcs));
		JsonObject calls = new JsonObject();
		calls.addProperty("bl_total", directCalls);
		calls.addProperty("bl_to_non_function", callToNonEntry);
		calls.addProperty("bl_to_function_without_fde", callToNoFde);
		calls.addProperty("distinct_bl_targets_without_fde", blTargetsNoFde.size());
		calls.addProperty("b_tailcalls_to_other_function", tailCalls);
		o.add("calls", calls);
		o.add("br", toJson(br));
		o.add("blr", toJson(blr));
		o.add("ret", toJson(ret));
		JsonObject jto = toJson(jt);
		jto.add("cases_per_table", distribution(jtCases, new long[] { 4, 8, 16, 32, 64, 128, 256 }));
		o.add("jumptables", jto);
		o.addProperty("address_taken_functions_from_code", taken.size());
		return o;
	}

	// ---------------------------------------------------------------- data pointers

	private JsonObject dataPointerStats(Set<Address> taken) throws Exception {
		long ptrToText = 0, ptrToFunc = 0, runs = 0, runSlots = 0, longestRun = 0;
		Set<Address> fromData = new HashSet<>();
		Address textMin = textSet.getMinAddress();
		Address textMax = textSet.getMaxAddress();
		AddressSpace space = textMin.getAddressSpace();
		for (MemoryBlock b : mem.getBlocks()) {
			if (b.isExecute() || !b.isInitialized() || b.isExternalBlock() ||
				!b.getStart().getAddressSpace().equals(space)) {
				continue;
			}
			long run = 0;
			Address a = b.getStart();
			long misalign = a.getOffset() & 7;
			if (misalign != 0) {
				a = a.add(8 - misalign);
			}
			byte[] buf = new byte[8];
			while (a.compareTo(b.getEnd()) < 0 && b.getEnd().subtract(a) >= 7) {
				monitor.checkCancelled();
				mem.getBytes(a, buf);
				long v = 0;
				for (int i = 7; i >= 0; i--) {
					v = (v << 8) | (buf[i] & 0xFF);
				}
				boolean isFunc = false;
				if (v >= textMin.getOffset() && v <= textMax.getOffset()) {
					ptrToText++;
					Address t = space.getAddress(v);
					Function f = fm.getFunctionAt(t);
					if (f != null) {
						ptrToFunc++;
						isFunc = true;
						fromData.add(t);
					}
				}
				if (isFunc) {
					run++;
				}
				else {
					if (run >= 2) {
						runs++;
						runSlots += run;
						longestRun = Math.max(longestRun, run);
					}
					run = 0;
				}
				a = a.add(8);
			}
			if (run >= 2) {
				runs++;
				runSlots += run;
				longestRun = Math.max(longestRun, run);
			}
		}
		Set<Address> all = new HashSet<>(taken);
		all.addAll(fromData);
		JsonObject o = new JsonObject();
		o.addProperty("slots_pointing_into_text", ptrToText);
		o.addProperty("slots_pointing_to_function_entry", ptrToFunc);
		o.addProperty("distinct_functions_referenced_from_data", fromData.size());
		o.addProperty("function_pointer_runs_len_ge2", runs);
		o.addProperty("function_pointer_run_slots", runSlots);
		o.addProperty("longest_function_pointer_run", longestRun);
		o.addProperty("address_taken_functions_total", all.size());
		return o;
	}

	// ---------------------------------------------------------------- imports

	private JsonObject importStats() throws Exception {
		Map<String, Long> calls = new HashMap<>();
		Map<String, Long> all = new HashMap<>();
		long thunkCount = 0;
		for (Function f : fm.getFunctions(true)) {
			monitor.checkCancelled();
			if (!f.isThunk() || f.isExternal()) {
				continue;
			}
			Function target = f.getThunkedFunction(true);
			if (target == null || !target.isExternal()) {
				continue;
			}
			thunkCount++;
			long n = 0;
			for (Reference r : refs.getReferencesTo(f.getEntryPoint())) {
				if (r.getReferenceType().isCall() || r.getReferenceType().isJump()) {
					n++;
				}
			}
			String name = target.getName(true).replaceFirst("^<EXTERNAL>::", "");
			inc(all, name, n);
			if (TRACKED_IMPORTS.contains(target.getName())) {
				inc(calls, target.getName(), n);
			}
		}
		JsonObject o = new JsonObject();
		o.addProperty("import_thunks", thunkCount);
		o.add("tracked_call_sites", toJson(calls));
		JsonObject top = new JsonObject();
		all.entrySet()
				.stream()
				.sorted((a, b) -> Long.compare(b.getValue(), a.getValue()))
				.limit(40)
				.forEach(e -> top.addProperty(e.getKey(), e.getValue()));
		o.add("top_imports_by_call_sites", top);
		return o;
	}
}
