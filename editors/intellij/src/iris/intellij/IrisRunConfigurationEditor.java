package iris.intellij;

import com.intellij.execution.configuration.EnvironmentVariablesComponent;
import com.intellij.openapi.fileChooser.FileChooserDescriptor;
import com.intellij.openapi.fileChooser.FileChooserDescriptorFactory;
import com.intellij.openapi.options.SettingsEditor;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.ui.ComboBox;
import com.intellij.openapi.ui.TextFieldWithBrowseButton;
import com.intellij.ui.RawCommandLineEditor;
import com.intellij.ui.components.JBTextField;
import com.intellij.util.ui.FormBuilder;

import javax.swing.JComponent;

final class IrisRunConfigurationEditor extends SettingsEditor<IrisRunConfiguration> {
    private final ComboBox<String> command = new ComboBox<>(IrisRunConfiguration.COMMANDS);
    private final TextFieldWithBrowseButton file = new TextFieldWithBrowseButton();
    private final JBTextField filter = new JBTextField();
    private final ComboBox<String> mode = new ComboBox<>(IrisRunConfiguration.MODES);
    private final RawCommandLineEditor irisOptions = new RawCommandLineEditor();
    private final RawCommandLineEditor programArgs = new RawCommandLineEditor();
    private final TextFieldWithBrowseButton workingDir = new TextFieldWithBrowseButton();
    private final EnvironmentVariablesComponent env;

    IrisRunConfigurationEditor(Project project) {
        env = new EnvironmentVariablesComponent(project);
        FileChooserDescriptor source = new FileChooserDescriptor(true, true, false, false, false, false)
            .withTitle("Iris File or Project Folder");
        file.addBrowseFolderListener(project, source);
        workingDir.addBrowseFolderListener(project, FileChooserDescriptorFactory.createSingleFolderDescriptor().withTitle("Working Directory"));
        filter.getEmptyText().setText("test and bench: only the blocks whose name has this");
        irisOptions.getEditorField().getEmptyText().setText("such as -o name, --cpu=native, --time=2s");
    }

    @Override
    protected void resetEditorFrom(IrisRunConfiguration c) {
        command.setSelectedItem(c.command);
        file.setText(c.file);
        filter.setText(c.filter);
        mode.setSelectedItem(c.mode);
        irisOptions.setText(c.irisOptions);
        programArgs.setText(c.programArgs);
        workingDir.setText(c.workingDir);
        env.setEnvData(c.env);
    }

    @Override
    protected void applyEditorTo(IrisRunConfiguration c) {
        c.command = (String) command.getSelectedItem();
        c.file = file.getText().trim();
        c.filter = filter.getText();
        c.mode = (String) mode.getSelectedItem();
        c.irisOptions = irisOptions.getText();
        c.programArgs = programArgs.getText();
        c.workingDir = workingDir.getText().trim();
        c.env = env.getEnvData();
    }

    @Override
    protected JComponent createEditor() {
        return FormBuilder.createFormBuilder()
            .addLabeledComponent("Command:", command)
            .addLabeledComponent("File or project:", file)
            .addLabeledComponent("Test or bench name:", filter)
            .addLabeledComponent("Mode:", mode)
            .addLabeledComponent("iris options:", irisOptions)
            .addLabeledComponent("Program arguments:", programArgs)
            .addLabeledComponent("Working directory:", workingDir)
            .addComponent(env)
            .getPanel();
    }
}
