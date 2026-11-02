package iris.intellij;

import com.intellij.openapi.fileChooser.FileChooserDescriptorFactory;
import com.intellij.openapi.options.Configurable;
import com.intellij.openapi.ui.TextFieldWithBrowseButton;
import com.intellij.util.ui.FormBuilder;

import javax.swing.JComponent;
import javax.swing.JLabel;

public final class IrisConfigurable implements Configurable {
    private TextFieldWithBrowseButton path;

    @Override
    public String getDisplayName() {
        return "Iris";
    }

    @Override
    public JComponent createComponent() {
        path = new TextFieldWithBrowseButton();
        path.addBrowseFolderListener(null, FileChooserDescriptorFactory.createSingleFileNoJarsDescriptor().withTitle("The iris Program"));
        return FormBuilder.createFormBuilder()
            .addLabeledComponent("iris program:", path)
            .addComponent(new JLabel("Empty: iris on PATH, then /usr/local/bin/iris. Used to run programs and for iris lsp."))
            .addComponentFillVertically(new javax.swing.JPanel(), 0)
            .getPanel();
    }

    @Override
    public boolean isModified() {
        return !path.getText().trim().equals(IrisSettings.get().irisPath());
    }

    @Override
    public void apply() {
        IrisSettings.get().setIrisPath(path.getText());
        IrisLsp.restartAll();
    }

    @Override
    public void reset() {
        path.setText(IrisSettings.get().irisPath());
    }

    @Override
    public void disposeUIResources() {
        path = null;
    }
}
