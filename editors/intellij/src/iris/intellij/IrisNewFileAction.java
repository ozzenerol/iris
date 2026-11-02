package iris.intellij;

import com.intellij.ide.actions.CreateFileFromTemplateAction;
import com.intellij.ide.actions.CreateFileFromTemplateDialog;
import com.intellij.openapi.project.DumbAware;
import com.intellij.openapi.project.Project;
import com.intellij.psi.PsiDirectory;

/** New, Iris File: a program with main, or an empty file. */
public final class IrisNewFileAction extends CreateFileFromTemplateAction implements DumbAware {
    public IrisNewFileAction() {
        super("Iris File", "Create an Iris file", IrisIcons.FILE);
    }

    @Override
    protected void buildDialog(Project project, PsiDirectory directory, CreateFileFromTemplateDialog.Builder builder) {
        builder.setTitle("New Iris File")
            .addKind("File", IrisIcons.FILE, "Iris File")
            .addKind("Program with main", IrisIcons.FILE, "Iris Program");
    }

    @Override
    protected String getActionName(PsiDirectory directory, String newName, String templateName) {
        return "Create Iris File " + newName;
    }
}
