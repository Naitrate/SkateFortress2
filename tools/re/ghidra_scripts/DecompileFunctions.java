// Create functions at the given addresses (bounds from .pdata) and write
// their decompilation. Args: <listfile> <outdir>. For quick targeted looks
// without creating all ~34k functions.
//@category tf2-skate
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.SourceType;
import java.nio.file.*;
import java.util.*;

public class DecompileFunctions extends GhidraScript {
    private final Map<Long, Long> pdata = new HashMap<>();

    private void loadPdata() throws Exception {
        Memory mem = currentProgram.getMemory();
        long base = 0x82000000L;
        long pe = base + (Integer.reverseBytes(mem.getInt(toAddr(base + 0x3C))) & 0xffffffffL);
        int sections = Short.reverseBytes(mem.getShort(toAddr(pe + 6))) & 0xffff;
        int optSize = Short.reverseBytes(mem.getShort(toAddr(pe + 20))) & 0xffff;
        for (int i = 0; i < sections; i++) {
            long sh = pe + 24 + optSize + 40L * i;
            byte[] name = new byte[8];
            mem.getBytes(toAddr(sh), name);
            if (!new String(name).startsWith(".pdata")) continue;
            long size = Integer.reverseBytes(mem.getInt(toAddr(sh + 8))) & 0xffffffffL;
            long va = base + (Integer.reverseBytes(mem.getInt(toAddr(sh + 12))) & 0xffffffffL);
            for (long o = 0; o < size; o += 8) {
                long begin = mem.getInt(toAddr(va + o)) & 0xffffffffL;
                long info = mem.getInt(toAddr(va + o + 4)) & 0xffffffffL;
                if (begin != 0) pdata.put(begin, ((info >> 8) & 0x3FFFFF) * 4);
            }
        }
    }

    private Function ensure(long start) {
        Address a = toAddr(start);
        Function f = getFunctionAt(a);
        if (f != null) return f;
        Long len = pdata.get(start);
        if (len == null) return null;
        AddressSet body = new AddressSet(a, a.add(len - 1));
        new DisassembleCommand(body, body, true).applyTo(currentProgram, monitor);
        new CreateFunctionCmd(null, a, body, SourceType.IMPORTED).applyTo(currentProgram, monitor);
        return getFunctionAt(a);
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        loadPdata();
        List<String> lines = Files.readAllLines(Paths.get(args[0]));
        // Create every listed function first so calls between them resolve.
        List<Function> funcs = new ArrayList<>();
        for (String line : lines) {
            line = line.trim();
            if (line.isEmpty() || line.startsWith("#")) continue;
            Function f = ensure(Long.parseLong(line.replace("0x", ""), 16));
            if (f != null) funcs.add(f); else println("no pdata function at " + line);
        }
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        Path out = Paths.get(args[1]);
        Files.createDirectories(out);
        for (Function f : funcs) {
            DecompileResults r = d.decompileFunction(f, 180, monitor);
            String c = r.decompileCompleted() ? r.getDecompiledFunction().getC() : ("// failed: " + r.getErrorMessage());
            Files.writeString(out.resolve(f.getEntryPoint().toString() + ".c"), c);
        }
        d.dispose();
        println("decompiled " + funcs.size());
    }
}
