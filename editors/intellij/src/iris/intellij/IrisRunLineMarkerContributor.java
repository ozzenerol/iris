package iris.intellij;

import com.intellij.execution.lineMarker.ExecutorAction;
import com.intellij.execution.lineMarker.RunLineMarkerContributor;
import com.intellij.icons.AllIcons;
import com.intellij.psi.PsiElement;
import com.intellij.psi.tree.IElementType;

/** A run button beside main, every test and every bench. */
public final class IrisRunLineMarkerContributor extends RunLineMarkerContributor {
    @Override
    public Info getInfo(PsiElement element) {
        if (element.getFirstChild() != null) {
            return null;
        }
        IElementType t = element.getNode().getElementType();
        String text = element.getText();
        boolean candidate = (t == IrisTokenTypes.FUNCTION && text.equals("main"))
            || (t == IrisTokenTypes.KEYWORD && (text.equals("test") || text.equals("bench")));
        if (!candidate) {
            return null;
        }
        IrisTarget target = IrisTarget.onLine(element);
        if (target == null) {
            return null;
        }
        var icon = target.command().equals("run") ? AllIcons.RunConfigurations.TestState.Run : AllIcons.RunConfigurations.TestState.Run_run;
        return new Info(icon, ExecutorAction.getActions(0), e -> target.command().equals("run") ? "Run" : "Run " + target.command() + " \"" + target.filter() + "\"");
    }
}
