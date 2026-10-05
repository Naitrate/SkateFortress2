// Create functions for every Xbox 360 .pdata entry (begin address + length),
// and mark .rdata/.data as non-executable. Headless post-import script.
//@category tf2-skate
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.mem.*;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;

public class CreatePdataFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        AddressSpace space = currentProgram.getAddressFactory().getDefaultAddressSpace();
        // Locate .pdata from the PE section table (works for any title update).
        long base = 0x82000000L;
        long pe = base + (Integer.reverseBytes(mem.getInt(space.getAddress(base + 0x3C))) & 0xffffffffL);
        int sections = Short.reverseBytes(mem.getShort(space.getAddress(pe + 6))) & 0xffff;
        int optSize = Short.reverseBytes(mem.getShort(space.getAddress(pe + 20))) & 0xffff;
        long pdata = 0, size = 0;
        for (int i = 0; i < sections; i++) {
            long sh = pe + 24 + optSize + 40L * i;
            byte[] name = new byte[8];
            mem.getBytes(space.getAddress(sh), name);
            if (new String(name).startsWith(".pdata")) {
                size = Integer.reverseBytes(mem.getInt(space.getAddress(sh + 8))) & 0xffffffffL;
                pdata = base + (Integer.reverseBytes(mem.getInt(space.getAddress(sh + 12))) & 0xffffffffL);
            }
        }
        println(String.format(".pdata at %x size %x", pdata, size));
        int made = 0;
        for (long off = 0; off < size; off += 8) {
            long begin = mem.getInt(space.getAddress(pdata + off)) & 0xffffffffL;
            long info = mem.getInt(space.getAddress(pdata + off + 4)) & 0xffffffffL;
            if (begin == 0) continue;
            long len = ((info >> 8) & 0x3FFFFF) * 4;
            Address a = space.getAddress(begin);
            AddressSet body = new AddressSet(a, a.add(len - 1));
            new DisassembleCommand(body, body, true).applyTo(currentProgram, monitor);
            if (getFunctionAt(a) == null) {
                CreateFunctionCmd cmd = new CreateFunctionCmd(null, a, body, ghidra.program.model.symbol.SourceType.IMPORTED);
                if (cmd.applyTo(currentProgram, monitor)) made++;
            }
            if ((off / 8) % 2000 == 0) println("pdata " + (off / 8) + " functions " + made);
        }
        println("created " + made + " functions");
    }
}
