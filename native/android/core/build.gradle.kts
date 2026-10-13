import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("org.jetbrains.kotlin.jvm")
}

java {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
    }
}

tasks.named<Test>("test") {
    enabled = false
}

val coreTest = tasks.register<JavaExec>("coreTest") {
    group = "verification"
    description = "Runs every *Test class in core with the in-tree test runner."
    val testSourceSet = sourceSets["test"]
    dependsOn(tasks.named("testClasses"))
    classpath = testSourceSet.runtimeClasspath
    mainClass.set("com.forkmesh.nativeapp.testing.TestRunnerKt")
    args(testSourceSet.output.classesDirs.asPath, (project.findProperty("filter") ?: "").toString())
}

tasks.named("check") {
    dependsOn(coreTest)
}
