// Headless post-script: dump every function of the current program.
//   decomp/raw/functions.tsv  addr size name class callees(imports) strings
//   decomp/raw/c/<addr>_<name>.c  Ghidra pseudo-C
// class: objc   = an Objective-C method (EAGLView, app delegate, ... -> rewritten, not ported)
//        gl/al/aq/ui = calls OpenGL ES / OpenAL / AudioQueue / UIKit+Foundation imports directly
//        game   = everything else (candidate for porting)
// Usage: analyzeHeadless <proj> <name> -import <bin> -postScript ExportFunctions.java <outdir>
//@category PixelRacing
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.Data;

import java.io.File;
import java.io.PrintWriter;
import java.util.TreeSet;

public class ExportFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args.length > 0 ? args[0] : "decomp/raw");
        File cdir = new File(out, "c");
        cdir.mkdirs();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        try (PrintWriter tsv = new PrintWriter(new File(out, "functions.tsv"), "UTF-8")) {
            tsv.println("addr\tsize\tname\tclass\tcallees\tstrings");
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            while (it.hasNext() && !monitor.isCancelled()) {
                Function f = it.next();
                if (f.isThunk() || f.isExternal()) continue;
                TreeSet<String> callees = new TreeSet<>();
                for (Function c : f.getCalledFunctions(monitor)) {
                    Function t = c.isThunk() ? c.getThunkedFunction(true) : c;
                    if (t != null && t.isExternal()) callees.add(t.getName());
                }
                TreeSet<String> strs = new TreeSet<>();
                for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
                    for (Reference r : ins.getReferencesFrom()) {
                        Data d = getDataAt(r.getToAddress());
                        if (d != null && d.hasStringValue()) {
                            String s = StringDataInstance.getStringDataInstance(d).getStringValue();
                            if (s != null && s.length() >= 3) strs.add(s.replaceAll("[\\t\\r\\n]", " "));
                        }
                    }
                }
                String name = f.getName(true);
                String cls = "game";
                if (name.startsWith("-[") || name.startsWith("+[")
                        || (!f.getParentNamespace().isGlobal() && f.getParentNamespace().getName().matches("[A-Z][A-Za-z_]+"))) cls = "objc";
                for (String c : callees) {
                    if (cls.equals("objc")) break;
                    if (c.startsWith("gl")) cls = "gl";
                    else if (c.startsWith("al")) cls = "al";
                    else if (c.startsWith("AudioQueue") || c.startsWith("AudioFile") || c.startsWith("AudioSession")) cls = "aq";
                    else if (c.startsWith("UI") || c.startsWith("NS") || c.startsWith("CF") || c.startsWith("CG")) cls = "ui";
                }
                tsv.printf("%s\t%d\t%s\t%s\t%s\t%s%n", f.getEntryPoint(), f.getBody().getNumAddresses(), name, cls,
                        String.join(",", callees), String.join(" | ", strs));
                DecompileResults r = di.decompileFunction(f, 60, monitor);
                if (r != null && r.decompileCompleted()) {
                    String fn = f.getEntryPoint() + "_" + f.getName().replaceAll("[^A-Za-z0-9_]", "_");
                    if (fn.length() > 120) fn = fn.substring(0, 120);
                    try (PrintWriter w = new PrintWriter(new File(cdir, fn + ".c"), "UTF-8")) {
                        w.print(r.getDecompiledFunction().getC());
                    }
                }
            }
        }
        di.dispose();
        println("ExportFunctions: done -> " + out.getAbsolutePath());
    }
}
