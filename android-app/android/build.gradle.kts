allprojects {
    repositories {
        google()
        mavenCentral()
    }
}

val newBuildDir: Directory =
    rootProject.layout.buildDirectory
        .dir("../../build")
        .get()
rootProject.layout.buildDirectory.value(newBuildDir)

subprojects {
    val newSubprojectBuildDir: Directory = newBuildDir.dir(project.name)
    project.layout.buildDirectory.value(newSubprojectBuildDir)
}

subprojects {
    afterEvaluate {
        val isAndroid = plugins.hasPlugin("com.android.application") ||
                        plugins.hasPlugin("com.android.library")
        if (isAndroid) {
            extensions.findByType<com.android.build.gradle.BaseExtension>()?.apply {
                if (namespace.isNullOrEmpty()) {
                    namespace = project.group.toString().ifEmpty {
                        "missing.namespace.${project.name}"
                    }
                }
                compileSdkVersion("android-34")
            }
        }
    }
}

subprojects {
    project.evaluationDependsOn(":app")
}

tasks.register<Delete>("clean") {
    delete(rootProject.layout.buildDirectory)
}
