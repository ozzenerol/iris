package iris.intellij;

import com.intellij.execution.configurations.ConfigurationFactory;
import com.intellij.execution.configurations.ConfigurationTypeBase;
import com.intellij.execution.configurations.ConfigurationTypeUtil;
import com.intellij.execution.configurations.RunConfiguration;
import com.intellij.openapi.components.BaseState;
import com.intellij.openapi.project.Project;

public final class IrisRunConfigurationType extends ConfigurationTypeBase {
    public static final String ID = "IrisRunConfiguration";

    public IrisRunConfigurationType() {
        super(ID, "Iris", "Run, test, bench or build an Iris program", IrisIcons.FILE);
        addFactory(new Factory(this));
    }

    public static IrisRunConfigurationType get() {
        return ConfigurationTypeUtil.findConfigurationType(IrisRunConfigurationType.class);
    }

    public ConfigurationFactory factory() {
        return getConfigurationFactories()[0];
    }

    static final class Factory extends ConfigurationFactory {
        Factory(IrisRunConfigurationType type) {
            super(type);
        }

        @Override
        public String getId() {
            return ID;
        }

        @Override
        public RunConfiguration createTemplateConfiguration(Project project) {
            return new IrisRunConfiguration(project, this, "Iris");
        }

        @Override
        public Class<? extends BaseState> getOptionsClass() {
            return null;
        }
    }
}
