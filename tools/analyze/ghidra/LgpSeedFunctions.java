// Pre-analysis script: creates a function at every .eh_frame FDE start listed in the
// sidecar JSON written by tools/analyze/nso2elf.py (--meta). Addresses in the JSON are
// module offsets; the program image base is added here.
//
// Usage (headless): -preScript LgpSeedFunctions.java <module.meta.json>
// @category LGP

import java.io.FileReader;
import java.io.Reader;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.FunctionManager;

public class LgpSeedFunctions extends GhidraScript {

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 1) {
			throw new IllegalArgumentException("usage: LgpSeedFunctions.java <module.meta.json>");
		}
		JsonObject meta;
		try (Reader r = new FileReader(args[0])) {
			meta = JsonParser.parseReader(r).getAsJsonObject();
		}
		Address base = currentProgram.getImageBase();
		FunctionManager fm = currentProgram.getFunctionManager();
		JsonArray fdes = meta.getAsJsonArray("fdes");
		int created = 0;
		int failed = 0;
		for (JsonElement e : fdes) {
			monitor.checkCancelled();
			long start = e.getAsJsonArray().get(0).getAsLong();
			Address a = base.add(start);
			if (fm.getFunctionAt(a) != null) {
				continue;
			}
			if (currentProgram.getListing().getInstructionAt(a) == null) {
				new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
			}
			if (new CreateFunctionCmd(a).applyTo(currentProgram, monitor)) {
				created++;
			}
			else {
				failed++;
			}
		}
		println(String.format("LgpSeedFunctions: %d FDEs, %d functions created, %d failed",
			fdes.size(), created, failed));
	}
}
