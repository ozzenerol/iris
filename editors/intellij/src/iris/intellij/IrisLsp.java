package iris.intellij;

import com.intellij.openapi.project.Project;
import com.intellij.openapi.project.ProjectManager;

/** Restarts iris lsp when it is on this IDE; does nothing where the LSP API is missing. */
final class IrisLsp {
    private IrisLsp() {
    }

    static void restartAll() {
        try {
            Class<?> c = Class.forName("iris.intellij.lsp.IrisLspServerSupportProvider");
            var m = c.getMethod("restart", Project.class);
            for (Project p : ProjectManager.getInstance().getOpenProjects()) {
                m.invoke(null, p);
            }
        } catch (ReflectiveOperationException | LinkageError ignored) {
            // no LSP support in this IDE
        }
    }
}
