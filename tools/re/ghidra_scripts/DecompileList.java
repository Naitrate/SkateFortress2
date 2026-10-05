// Decompile functions listed (hex addresses, one per line) in args[0] into args[1]/<addr>.c
//@category tf2-skate
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.Function;
import java.io.*;
import java.nio.file.*;

public class DecompileList extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        Path out = Paths.get(args[1]);
        Files.createDirectories(out);
        for (String line : Files.readAllLines(Paths.get(args[0]))) {
            line = line.trim();
            if (line.isEmpty()) continue;
            Function f = getFunctionContaining(toAddr(Long.parseLong(line.replace("0x", ""), 16)));
            if (f == null) { println("no function at " + line); continue; }
            DecompileResults r = d.decompileFunction(f, 120, monitor);
            String c = r.decompileCompleted() ? r.getDecompiledFunction().getC() : ("// failed: " + r.getErrorMessage());
            Files.writeString(out.resolve(f.getEntryPoint().toString() + ".c"), c);
        }
        d.dispose();
    }
}
