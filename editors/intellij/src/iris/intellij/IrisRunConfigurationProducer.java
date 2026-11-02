package iris.intellij;

import com.intellij.execution.actions.ConfigurationContext;
import com.intellij.execution.actions.LazyRunConfigurationProducer;
import com.intellij.execution.configurations.ConfigurationFactory;
import com.intellij.openapi.util.Ref;
import com.intellij.openapi.vfs.VirtualFile;
import com.intellij.psi.PsiElement;
import com.intellij.psi.PsiFile;

/** Makes the run configuration from the file, test or bench the user runs. */
public final class IrisRunConfigurationProducer extends LazyRunConfigurationProducer<IrisRunConfiguration> {
    @Override
    public ConfigurationFactory getConfigurationFactory() {
        return IrisRunConfigurationType.get().factory();
    }

    private static IrisTarget target(ConfigurationContext context) {
        PsiElement element = context.getPsiLocation();
        if (element == null || !(element.getContainingFile() instanceof IrisFile file)) {
            return null;
        }
        IrisTarget t = element instanceof PsiFile ? null : IrisTarget.onLine(element);
        return t != null ? t : IrisTarget.ofFile(file);
    }

    private static String path(ConfigurationContext context) {
        PsiElement element = context.getPsiLocation();
        PsiFile file = element == null ? null : element.getContainingFile();
        VirtualFile vf = file == null ? null : file.getVirtualFile();
        return vf == null ? null : vf.getPath();
    }

    @Override
    protected boolean setupConfigurationFromContext(IrisRunConfiguration c, ConfigurationContext context, Ref<PsiElement> source) {
        IrisTarget t = target(context);
        String path = path(context);
        if (t == null || path == null) {
            return false;
        }
        c.command = t.command();
        c.filter = t.filter();
        c.file = path;
        c.setName(c.suggestedName());
        return true;
    }

    @Override
    public boolean isConfigurationFromContext(IrisRunConfiguration c, ConfigurationContext context) {
        IrisTarget t = target(context);
        String path = path(context);
        return t != null && path != null && path.equals(c.file)
            && t.command().equals(c.command) && t.filter().equals(c.filter);
    }
}
