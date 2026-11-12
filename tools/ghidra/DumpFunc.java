// Dump disassembly + decompilation of the functions given as script args (hex addresses),
// to <outdir>/<addr>.txt. Usage: -postScript DumpFunc.java <outdir> <addr> [addr...]
//@category PixelRacing
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import java.io.*;

public class DumpFunc extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        File out = new File(a[0]);
        out.mkdirs();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        for (int i = 1; i < a.length; i++) {
            Address ad = toAddr(a[i]);
            Function f = getFunctionContaining(ad);
            if (f == null) f = createFunction(ad, null);
            try (PrintWriter w = new PrintWriter(new File(out, a[i] + ".txt"))) {
                if (f == null) { w.println("no function"); continue; }
                w.println("// " + f.getName() + " " + f.getBody());
                InstructionIterator ii = currentProgram.getListing().getInstructions(f.getBody(), true);
                while (ii.hasNext()) { Instruction in = ii.next(); w.println(in.getAddress() + "  " + in); }
                DecompileResults r = di.decompileFunction(f, 600, monitor);
                if (r.decompileCompleted()) w.print(r.getDecompiledFunction().getC());
                else w.println("DECOMPILE FAILED: " + r.getErrorMessage());
            }
        }
    }
}
