// Post-analysis fix-ups for the iOS armv7 slice, run before ExportFunctions.java:
//  * iOS armv7 uses softfp (floats in core registers): every function, imports included, gets
//    the __stdcall_softfp prototype, and the libm imports get their real float signatures.
//  * The non-lazy pointer / const pointer sections are marked read-only so the decompiler folds
//    *PTR_DAT_xxx into direct references to the globals.
//@category PixelRacing
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

import java.util.ArrayList;
import java.util.List;

public class FixupIOS extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            String n = b.getName();
            if (n.contains("nl_symbol_ptr") || n.contains("la_symbol_ptr") || n.equals("__const")
                    || n.contains("objc_classrefs") || n.contains("objc_selrefs") || n.contains("cfstring")) {
                b.setWrite(false);
                println("read-only: " + n + " " + b.getStart() + "-" + b.getEnd());
            }
        }
        int n = 0;
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        while (it.hasNext()) {
            Function f = it.next();
            try { f.setCallingConvention("__stdcall_softfp"); n++; } catch (Exception e) { /* keep */ }
        }
        for (Function f : currentProgram.getFunctionManager().getExternalFunctions()) {
            try { f.setCallingConvention("__stdcall_softfp"); n++; } catch (Exception e) { /* keep */ }
            String name = f.getName().replaceFirst("^_", "");
            DataType fl = FloatDataType.dataType, db = DoubleDataType.dataType;
            switch (name) {
                case "sinf": case "cosf": case "sqrtf": case "tanf": case "acosf": case "asinf":
                case "atanf": case "fabsf": case "floorf": case "ceilf": case "expf": case "logf":
                    sig(f, fl, fl); break;
                case "atan2f": case "powf": case "fmodf":
                    sig(f, fl, fl, fl); break;
                case "sin": case "cos": case "sqrt": case "acos": case "asin": case "atan": case "fabs":
                case "floor": case "ceil": case "exp": case "log":
                    sig(f, db, db); break;
                case "atan2": case "pow": case "fmod":
                    sig(f, db, db, db); break;
                default: break;
            }
        }
        println("softfp prototypes set: " + n);
    }

    private void sig(Function f, DataType ret, DataType... args) throws Exception {
        List<ParameterImpl> ps = new ArrayList<>();
        for (int i = 0; i < args.length; i++) ps.add(new ParameterImpl("a" + i, args[i], currentProgram));
        f.updateFunction("__stdcall_softfp", new ReturnParameterImpl(ret, currentProgram), ps,
                Function.FunctionUpdateType.DYNAMIC_STORAGE_ALL_PARAMS, true, SourceType.USER_DEFINED);
    }
}
