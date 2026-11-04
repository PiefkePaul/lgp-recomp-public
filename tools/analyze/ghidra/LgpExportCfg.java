// Exports Ghidra's view of function entries and BR sites as module offsets (relative to the
// program image base), for cross-checking lgp_recomp's CFG (tools/analyze/ghidra/compare_recomp.py).
// The output holds addresses and counts only, no code bytes; it stays in $LGP_PRIVATE_DIR.
//
// Usage (headless, existing project): -process <prog> -noanalysis -postScript LgpExportCfg.java <out.json>
// @category LGP

import java.io.FileWriter;
import java.io.Writer;
import java.util.*;

import com.google.gson.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

public class LgpExportCfg extends GhidraScript {

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 1) {
			throw new IllegalArgumentException("usage: LgpExportCfg.java <out.json>");
		}
		Address base = currentProgram.getImageBase();
		FunctionManager fm = currentProgram.getFunctionManager();
		JsonArray funcs = new JsonArray();
		for (Function f : fm.getFunctions(true)) {
			monitor.checkCancelled();
			if (f.isExternal()) {
				continue;
			}
			JsonArray e = new JsonArray();
			e.add(f.getEntryPoint().subtract(base));
			e.add(f.getBody().getNumAddresses());
			e.add(f.isThunk() ? 1 : 0);
			funcs.add(e);
		}
		JsonArray brs = new JsonArray();
		InstructionIterator it = currentProgram.getListing().getInstructions(true);
		while (it.hasNext()) {
			Instruction in = it.next();
			String m = in.getMnemonicString().toLowerCase();
			if (!m.equals("br")) {
				continue;
			}
			Function here = fm.getFunctionContaining(in.getAddress());
			int computed = 0;
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
				else {
					computed++;
				}
			}
			String kind = here != null && here.isThunk() ? "thunk"
					: computed > 1 ? "jumptable" : toFunction ? "tailcall" : "unresolved";
			JsonArray e = new JsonArray();
			e.add(in.getAddress().subtract(base));
			e.add(kind);
			e.add(computed);
			brs.add(e);
		}
		JsonObject out = new JsonObject();
		out.addProperty("program", currentProgram.getName());
		out.add("functions", funcs);
		out.add("br", brs);
		try (Writer w = new FileWriter(args[0])) {
			new Gson().toJson(out, w);
		}
		println("LgpExportCfg: " + funcs.size() + " functions, " + brs.size() + " BR -> " + args[0]);
	}
}
