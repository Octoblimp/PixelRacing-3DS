// Post-analysis fix-up for the Wii DOL: give the decompiler the EABI small-data bases
// (r2 = _SDA2_BASE_, r13 = _SDA_BASE_, set once in __init_registers) for every address,
// so r13/r2-relative loads fold into direct global references. Run before ExportFunctions.java.
//@category PixelRacing
import ghidra.app.script.GhidraScript;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.ProgramContext;
import ghidra.program.model.mem.MemoryBlock;
import java.math.BigInteger;

public class FixupWii extends GhidraScript {
    @Override
    public void run() throws Exception {
        ProgramContext pc = currentProgram.getProgramContext();
        Register r2 = currentProgram.getRegister("r2"), r13 = currentProgram.getRegister("r13");
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            if (!b.isExecute()) continue;
            pc.setValue(r2, b.getStart(), b.getEnd(), BigInteger.valueOf(0x80173760L));
            pc.setValue(r13, b.getStart(), b.getEnd(), BigInteger.valueOf(0x80172280L));
            println("SDA bases set on " + b.getName());
        }
    }
}
