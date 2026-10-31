// Headless: decompile every function and write one C file plus a function index.
// Usage: analyzeHeadless <proj> <name> -import X.EXE -postScript ExportDecomp.java <outdir>
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;

public class ExportDecomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outDir = getScriptArgs().length > 0 ? getScriptArgs()[0] : ".";
        String base = currentProgram.getName().replace('.', '_');
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        try (PrintWriter c = new PrintWriter(new File(outDir, base + ".c"));
             PrintWriter idx = new PrintWriter(new File(outDir, base + "_functions.tsv"));
             PrintWriter str = new PrintWriter(new File(outDir, base + "_strings.tsv"))) {
            idx.println("addr\tname\tsize\tcallers\tcallees");
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (monitor.isCancelled()) break;
                DecompileResults r = di.decompileFunction(f, 120, monitor);
                c.println("// ==== " + f.getEntryPoint() + " " + f.getName() + " ====");
                c.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage());
                idx.println(f.getEntryPoint() + "\t" + f.getName() + "\t" + f.getBody().getNumAddresses() + "\t"
                    + f.getCallingFunctions(monitor).size() + "\t" + f.getCalledFunctions(monitor).size());
            }
            for (Data d : currentProgram.getListing().getDefinedData(true)) {
                if (d.hasStringValue()) {
                    StringBuilder refs = new StringBuilder();
                    for (Reference ref : getReferencesTo(d.getAddress())) {
                        Function f = getFunctionContaining(ref.getFromAddress());
                        refs.append(f == null ? ref.getFromAddress().toString() : f.getName()).append(' ');
                    }
                    str.println(d.getAddress() + "\t" + String.valueOf(d.getValue()).replace("\n", "\\n").replace("\t", "\\t") + "\t" + refs);
                }
            }
        }
        di.dispose();
    }
}
