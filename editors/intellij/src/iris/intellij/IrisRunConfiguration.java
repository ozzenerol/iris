package iris.intellij;

import com.intellij.execution.ExecutionException;
import com.intellij.execution.Executor;
import com.intellij.execution.configuration.EnvironmentVariablesData;
import com.intellij.execution.configurations.CommandLineState;
import com.intellij.execution.configurations.ConfigurationFactory;
import com.intellij.execution.configurations.GeneralCommandLine;
import com.intellij.execution.configurations.LocatableConfigurationBase;
import com.intellij.execution.configurations.RunConfiguration;
import com.intellij.execution.configurations.RunProfileState;
import com.intellij.execution.configurations.RuntimeConfigurationError;
import com.intellij.execution.configurations.RuntimeConfigurationException;
import com.intellij.execution.process.KillableColoredProcessHandler;
import com.intellij.execution.process.ProcessHandler;
import com.intellij.execution.process.ProcessTerminatedListener;
import com.intellij.execution.runners.ExecutionEnvironment;
import com.intellij.openapi.options.SettingsEditor;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.util.InvalidDataException;
import com.intellij.openapi.util.JDOMExternalizerUtil;
import com.intellij.util.execution.ParametersListUtil;
import org.jdom.Element;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

/** iris run|test|bench|build|check [--mode=...] [options] <file> [name] [args] */
public final class IrisRunConfiguration extends LocatableConfigurationBase<RunProfileState> {
    public static final String[] COMMANDS = {"run", "test", "bench", "build", "check"};
    public static final String[] MODES = {"debug", "release-safe", "release-fast"};

    public String command = "run";
    public String file = "";
    public String filter = "";
    public String mode = "debug";
    public String irisOptions = "";
    public String programArgs = "";
    public String workingDir = "";
    public EnvironmentVariablesData env = EnvironmentVariablesData.DEFAULT;

    IrisRunConfiguration(Project project, ConfigurationFactory factory, String name) {
        super(project, factory, name);
    }

    @Override
    public SettingsEditor<? extends RunConfiguration> getConfigurationEditor() {
        return new IrisRunConfigurationEditor(getProject());
    }

    @Override
    public void checkConfiguration() throws RuntimeConfigurationException {
        if (file.isBlank()) {
            throw new RuntimeConfigurationError("Choose the .iris file or project folder to " + command);
        }
        if (!new File(file).exists()) {
            throw new RuntimeConfigurationError(file + " does not exist");
        }
    }

    @Override
    public String suggestedName() {
        String base = new File(file).getName();
        if (!filter.isBlank()) {
            return command + " \"" + filter + "\"";
        }
        return command.equals("run") ? base : command + " " + base;
    }

    public GeneralCommandLine commandLine() {
        List<String> args = new ArrayList<>();
        args.add(IrisSettings.get().iris());
        args.add(command);
        if (!command.equals("check") && !mode.equals("debug") && !mode.isBlank()) {
            args.add("--mode=" + mode);
        }
        args.addAll(ParametersListUtil.parse(irisOptions));
        args.add(file);
        if ((command.equals("test") || command.equals("bench")) && !filter.isBlank()) {
            args.add(filter);
        }
        if (command.equals("run")) {
            args.addAll(ParametersListUtil.parse(programArgs));
        }
        GeneralCommandLine cl = new GeneralCommandLine(args).withCharset(java.nio.charset.StandardCharsets.UTF_8);
        String dir = workingDir.isBlank() ? defaultDir() : workingDir;
        if (dir != null) {
            cl.withWorkDirectory(dir);
        }
        cl.withEnvironment(env.getEnvs());
        cl.withParentEnvironmentType(env.isPassParentEnvs()
            ? GeneralCommandLine.ParentEnvironmentType.CONSOLE : GeneralCommandLine.ParentEnvironmentType.NONE);
        return cl;
    }

    private String defaultDir() {
        File f = new File(file);
        if (f.isDirectory()) {
            return f.getPath();
        }
        if (f.getParentFile() != null) {
            return f.getParent();
        }
        return getProject().getBasePath();
    }

    @Override
    public RunProfileState getState(Executor executor, ExecutionEnvironment environment) throws ExecutionException {
        return new CommandLineState(environment) {
            @Override
            protected ProcessHandler startProcess() throws ExecutionException {
                KillableColoredProcessHandler handler = new KillableColoredProcessHandler(commandLine());
                ProcessTerminatedListener.attach(handler);
                return handler;
            }
        };
    }

    @Override
    public void readExternal(Element element) throws InvalidDataException {
        super.readExternal(element);
        command = read(element, "command", "run");
        file = read(element, "file", "");
        filter = read(element, "filter", "");
        mode = read(element, "mode", "debug");
        irisOptions = read(element, "irisOptions", "");
        programArgs = read(element, "programArgs", "");
        workingDir = read(element, "workingDir", "");
        env = EnvironmentVariablesData.readExternal(element);
    }

    @Override
    public void writeExternal(Element element) {
        super.writeExternal(element);
        JDOMExternalizerUtil.writeField(element, "command", command);
        JDOMExternalizerUtil.writeField(element, "file", file);
        JDOMExternalizerUtil.writeField(element, "filter", filter);
        JDOMExternalizerUtil.writeField(element, "mode", mode);
        JDOMExternalizerUtil.writeField(element, "irisOptions", irisOptions);
        JDOMExternalizerUtil.writeField(element, "programArgs", programArgs);
        JDOMExternalizerUtil.writeField(element, "workingDir", workingDir);
        env.writeExternal(element);
    }

    private static String read(Element element, String name, String fallback) {
        String v = JDOMExternalizerUtil.readField(element, name);
        return v == null ? fallback : v;
    }
}
