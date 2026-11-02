package iris.intellij.lsp;

import com.intellij.execution.configurations.GeneralCommandLine;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.vfs.VirtualFile;
import com.intellij.platform.lsp.api.LspClientManager;
import com.intellij.platform.lsp.api.LspIntegrationProvider;
import com.intellij.platform.lsp.api.ProjectWideLspClientDescriptor;
import iris.intellij.IrisSettings;

/** Starts iris lsp for errors, hover, go to definition, completion and formatting. */
public final class IrisLspServerSupportProvider implements LspIntegrationProvider {
    @Override
    public void fileOpened(Project project, VirtualFile file, LspClientStarter starter) {
        if ("iris".equals(file.getExtension())) {
            starter.ensureClientStarted(new Descriptor(project));
        }
    }

    public static void restart(Project project) {
        LspClientManager.getInstance(project).stopAndRestartClientsIfNeeded(IrisLspServerSupportProvider.class);
    }

    static final class Descriptor extends ProjectWideLspClientDescriptor {
        Descriptor(Project project) {
            super(project, "Iris");
        }

        @Override
        public boolean isSupportedFile(VirtualFile file) {
            return "iris".equals(file.getExtension());
        }

        @Override
        public String getLanguageId(VirtualFile file) {
            return "iris";
        }

        @Override
        public GeneralCommandLine createCommandLine() {
            GeneralCommandLine cl = new GeneralCommandLine(IrisSettings.get().iris(), "lsp");
            if (getProject().getBasePath() != null) {
                cl.withWorkDirectory(getProject().getBasePath());
            }
            return cl;
        }
    }
}
