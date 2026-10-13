#!/usr/bin/env ruby
# frozen_string_literal: true

# Writes native/ios/ForkMesh.xcodeproj from scratch.
#
# The project is generated rather than hand-edited because nobody here can
# open Xcode: this script is the project's source, it runs on Linux as well as
# on a Mac, and Codemagic runs it again before every build so the checked-in
# copy can never drift from it. Source folders are file-system synchronized
# groups (Xcode 16+), so adding or moving a Swift file changes nothing here.
#
#   gem install --user-install xcodeproj   # already present wherever CocoaPods is
#   ruby native/ios/tools/generate_project.rb

require 'fileutils'
require 'xcodeproj'

ROOT = File.expand_path('..', __dir__)
PROJECT_PATH = File.join(ROOT, 'ForkMesh.xcodeproj')

TEAM_ID = '742ZANCB57'
APP_BUNDLE_ID = 'com.forkmesh.forkmesh'
EXTENSION_BUNDLE_ID = "#{APP_BUNDLE_ID}.NotificationService"
DEPLOYMENT_TARGET = '17.0'
# The watch app (docs/native/PARITY.md, section 4) and its widget extension.
WATCH_BUNDLE_ID = "#{APP_BUNDLE_ID}.watchkitapp"
WATCH_WIDGETS_BUNDLE_ID = "#{WATCH_BUNDLE_ID}.widgets"
WATCH_DEPLOYMENT_TARGET = '10.0'
# Gate 2 (docs/native/DESKTOP_PARITY.md, 19.4): the phone's own extensions.
WIDGETS_BUNDLE_ID = "#{APP_BUNDLE_ID}.Widgets"
SHARE_BUNDLE_ID = "#{APP_BUNDLE_ID}.Share"
AUTOFILL_BUNDLE_ID = "#{APP_BUNDLE_ID}.AutoFill"
# Synchronized groups need the Xcode 16 project format.
OBJECT_VERSION = 77

Obj = Xcodeproj::Project::Object

# Object ids are handed out in creation order instead of at random, so
# regenerating an unchanged project is not a diff.
module Xcodeproj
  class Project
    private

    def generate_available_uuid_list(count = 100)
      @forkmesh_next_id ||= 0
      fresh = Array.new(count) { format('F0%022X', @forkmesh_next_id += 1) }
      @generated_uuids += fresh
      @available_uuids += fresh
    end
  end
end

FileUtils.rm_rf(PROJECT_PATH)
project = Xcodeproj::Project.new(PROJECT_PATH, false, OBJECT_VERSION)
project.root_object.attributes = {
  'BuildIndependentTargetsInParallel' => '1',
  'LastSwiftUpdateCheck' => '1600',
  'LastUpgradeCheck' => '1600',
}
# The gem defaults this to its own newest known format; keep it on the one
# actually written, which is what Xcode 16 itself records.
project.root_object.preferred_project_object_version = OBJECT_VERSION.to_s
project.root_object.development_region = 'en'
project.root_object.known_regions = %w[en Base]

version_config = project.main_group.new_group('Config', 'Config')
version_xcconfig = version_config.new_file('Version.xcconfig')
%w[
  ForkMesh-Info.plist ForkMesh-Debug.entitlements ForkMesh-Release.entitlements
  NotificationService-Info.plist NotificationService.entitlements ExportOptions.plist
].each { |name| version_config.new_file(name) }

project.build_configurations.each do |config|
  config.build_settings.merge!(
    'ALWAYS_SEARCH_USER_PATHS' => 'NO',
    'CLANG_ENABLE_MODULES' => 'YES',
    'CLANG_ENABLE_OBJC_ARC' => 'YES',
    'DEVELOPMENT_TEAM' => TEAM_ID,
    'ENABLE_USER_SCRIPT_SANDBOXING' => 'YES',
    'IPHONEOS_DEPLOYMENT_TARGET' => DEPLOYMENT_TARGET,
    'SDKROOT' => 'iphoneos',
    'SWIFT_VERSION' => '5.0',
    'TARGETED_DEVICE_FAMILY' => '1,2'
  )
  if config.name == 'Debug'
    config.build_settings.merge!(
      'DEBUG_INFORMATION_FORMAT' => 'dwarf',
      'ENABLE_TESTABILITY' => 'YES',
      'ONLY_ACTIVE_ARCH' => 'YES',
      'SWIFT_ACTIVE_COMPILATION_CONDITIONS' => 'DEBUG $(inherited)',
      'SWIFT_OPTIMIZATION_LEVEL' => '-Onone'
    )
  else
    config.build_settings.merge!(
      # dSYMs are what make a TestFlight crash log readable.
      'DEBUG_INFORMATION_FORMAT' => 'dwarf-with-dsym',
      'SWIFT_COMPILATION_MODE' => 'wholemodule',
      'SWIFT_OPTIMIZATION_LEVEL' => '-O',
      'VALIDATE_PRODUCT' => 'YES'
    )
  end
end

def synchronized_group(project, path)
  group = project.new(Obj::PBXFileSystemSynchronizedRootGroup)
  group.path = path
  group.source_tree = '<group>'
  project.main_group.children << group
  group
end

def native_target(project, name, product_type, product_name, platform = :ios, deployment_target = DEPLOYMENT_TARGET)
  target = project.new(Obj::PBXNativeTarget)
  project.targets << target
  target.name = name
  target.product_name = name
  target.product_type = Xcodeproj::Constants::PRODUCT_TYPE_UTI[product_type]
  target.build_configuration_list = Xcodeproj::Project::ProjectHelper.configuration_list(
    project, platform, deployment_target, product_type, :swift
  )
  product = project.products_group.new_reference(product_name, :built_products)
  product.include_in_index = '0'
  product.set_explicit_file_type
  target.product_reference = product
  target.build_phases << project.new(Obj::PBXSourcesBuildPhase)
  target.build_phases << project.new(Obj::PBXFrameworksBuildPhase)
  target.build_phases << project.new(Obj::PBXResourcesBuildPhase)
  target
end

def configure(target, version_xcconfig)
  target.build_configurations.each do |config|
    config.base_configuration_reference = version_xcconfig
    # The helper seeds settings meant for Xcode's templates; start clean so
    # every setting this project relies on is visible below.
    config.build_settings.clear
    config.build_settings.merge!(
      'CODE_SIGN_STYLE' => 'Automatic',
      'GENERATE_INFOPLIST_FILE' => 'NO',
      'PRODUCT_NAME' => '$(TARGET_NAME)',
      'SWIFT_EMIT_LOC_STRINGS' => 'YES'
    )
    yield config
  end
end

# --- The app ---------------------------------------------------------------

# Compiled into both targets: what the app and its extension must agree on.
shared_sources = synchronized_group(project, 'Shared')

app = native_target(project, 'ForkMesh', :application, 'ForkMesh.app')
app.file_system_synchronized_groups << synchronized_group(project, 'ForkMesh')
app.file_system_synchronized_groups << shared_sources
configure(app, version_xcconfig) do |config|
  release = config.name != 'Debug'
  config.build_settings.merge!(
    'ASSETCATALOG_COMPILER_APPICON_NAME' => 'AppIcon',
    'ASSETCATALOG_COMPILER_GENERATE_SWIFT_ASSET_SYMBOL_EXTENSIONS' => 'NO',
    # Debug builds talk to the APNs sandbox, release builds to production.
    'FORKMESH_APP_ENTITLEMENTS' => release ? 'Release' : 'Debug',
    'CODE_SIGN_ENTITLEMENTS' => 'Config/ForkMesh-$(FORKMESH_APP_ENTITLEMENTS).entitlements',
    'INFOPLIST_FILE' => 'Config/ForkMesh-Info.plist',
    'LD_RUNPATH_SEARCH_PATHS' => ['$(inherited)', '@executable_path/Frameworks'],
    'PRODUCT_BUNDLE_IDENTIFIER' => APP_BUNDLE_ID
  )
end

# ForkMeshCore is a local Swift package beside the project.
core_package = project.new(Obj::XCLocalSwiftPackageReference)
core_package.relative_path = 'ForkMeshCore'
project.root_object.package_references << core_package
core_product = project.new(Obj::XCSwiftPackageProductDependency)
core_product.product_name = 'ForkMeshCore'
app.package_product_dependencies << core_product
core_build_file = project.new(Obj::PBXBuildFile)
core_build_file.product_ref = core_product
app.frameworks_build_phase.files << core_build_file

# --- The notification service extension ------------------------------------

extension = native_target(project, 'NotificationService', :app_extension, 'NotificationService.appex')
extension.file_system_synchronized_groups << synchronized_group(project, 'NotificationService')
extension.file_system_synchronized_groups << shared_sources
configure(extension, version_xcconfig) do |config|
  config.build_settings.merge!(
    'CODE_SIGN_ENTITLEMENTS' => 'Config/NotificationService.entitlements',
    'INFOPLIST_FILE' => 'Config/NotificationService-Info.plist',
    'LD_RUNPATH_SEARCH_PATHS' => [
      '$(inherited)', '@executable_path/Frameworks', '@executable_path/../../Frameworks'
    ],
    'PRODUCT_BUNDLE_IDENTIFIER' => EXTENSION_BUNDLE_ID,
    'SKIP_INSTALL' => 'YES'
  )
end

app.add_dependency(extension)
embed = app.new_copy_files_build_phase('Embed Foundation Extensions')
embed.symbol_dst_subfolder_spec = :plug_ins
embed_file = embed.add_file_reference(extension.product_reference)
embed_file.settings = { 'ATTRIBUTES' => ['RemoveHeadersOnCopy'] }

# --- On-device models: the one third-party dependency ----------------------

# MLX runs Gemma 4 on the phone's GPU (ForkMesh/Features/LocalAI) and
# swift-transformers reads its tokenizer and chat template; README.md,
# "Dependency policy", records why. App target only: the notification
# extension must stay small. mlx-swift is listed for its MLX product (the
# buffer-cache limit); the version range is the one mlx-swift-lm itself asks
# for. Created last so the ids above stay put.
REMOTE_PACKAGES = [
  { url: 'https://github.com/ml-explore/mlx-swift-lm', from: '3.31.4', products: %w[MLXLLM MLXLMCommon] },
  { url: 'https://github.com/ml-explore/mlx-swift', from: '0.31.4', products: %w[MLX] },
  { url: 'https://github.com/huggingface/swift-transformers', from: '1.3.4', products: %w[Tokenizers] },
].freeze

REMOTE_PACKAGES.each do |spec|
  reference = project.new(Obj::XCRemoteSwiftPackageReference)
  reference.repositoryURL = spec[:url]
  reference.requirement = { 'kind' => 'upToNextMinorVersion', 'minimumVersion' => spec[:from] }
  project.root_object.package_references << reference
  spec[:products].each do |name|
    product = project.new(Obj::XCSwiftPackageProductDependency)
    product.package = reference
    product.product_name = name
    app.package_product_dependencies << product
    build_file = project.new(Obj::PBXBuildFile)
    build_file.product_ref = product
    app.frameworks_build_phase.files << build_file
  end
end

# --- The watch app and its widgets (docs/native/PARITY.md, 4.5) -----------

# Created after everything above so the ids of the existing objects stay put.
# Both link ForkMeshWatchCore, the package's watchOS-safe product (FMBase,
# FMAgentWatch, FMWatchWire); never MLX.
def watch_settings(config, entitlements, info_plist, bundle_id)
  config.build_settings.merge!(
    'ASSETCATALOG_COMPILER_GENERATE_SWIFT_ASSET_SYMBOL_EXTENSIONS' => 'NO',
    'CODE_SIGN_ENTITLEMENTS' => entitlements,
    'INFOPLIST_FILE' => info_plist,
    'PRODUCT_BUNDLE_IDENTIFIER' => bundle_id,
    'SDKROOT' => 'watchos',
    # Installed inside the iOS app, never on its own.
    'SKIP_INSTALL' => 'YES',
    'TARGETED_DEVICE_FAMILY' => '4',
    'WATCHOS_DEPLOYMENT_TARGET' => WATCH_DEPLOYMENT_TARGET
  )
end

def link_watch_core(project, target)
  product = project.new(Obj::XCSwiftPackageProductDependency)
  product.product_name = 'ForkMeshWatchCore'
  target.package_product_dependencies << product
  build_file = project.new(Obj::PBXBuildFile)
  build_file.product_ref = product
  target.frameworks_build_phase.files << build_file
end

# FORKMESH_NATIVE_WATCH=0 builds the phone app without the watch targets.
if ENV.fetch('FORKMESH_NATIVE_WATCH', '1') != '0'
%w[
  ForkMeshWatch-Info.plist ForkMeshWatch.entitlements
  ForkMeshWatchWidgets-Info.plist ForkMeshWatchWidgets.entitlements
].each { |name| version_config.new_file(name) }

watch = native_target(project, 'ForkMeshWatch', :application, 'ForkMeshWatch.app', :watchos, WATCH_DEPLOYMENT_TARGET)
watch.file_system_synchronized_groups << synchronized_group(project, 'ForkMeshWatch')
configure(watch, version_xcconfig) do |config|
  watch_settings(config, 'Config/ForkMeshWatch.entitlements', 'Config/ForkMeshWatch-Info.plist', WATCH_BUNDLE_ID)
  config.build_settings.merge!(
    'ASSETCATALOG_COMPILER_APPICON_NAME' => 'AppIcon',
    'LD_RUNPATH_SEARCH_PATHS' => ['$(inherited)', '@executable_path/Frameworks']
  )
end
link_watch_core(project, watch)

widgets = native_target(
  project, 'ForkMeshWatchWidgets', :app_extension, 'ForkMeshWatchWidgets.appex', :watchos, WATCH_DEPLOYMENT_TARGET
)
widgets.file_system_synchronized_groups << synchronized_group(project, 'ForkMeshWatchWidgets')
configure(widgets, version_xcconfig) do |config|
  watch_settings(
    config, 'Config/ForkMeshWatchWidgets.entitlements', 'Config/ForkMeshWatchWidgets-Info.plist', WATCH_WIDGETS_BUNDLE_ID
  )
  config.build_settings.merge!(
    'LD_RUNPATH_SEARCH_PATHS' => [
      '$(inherited)', '@executable_path/Frameworks', '@executable_path/../../Frameworks'
    ]
  )
end
link_watch_core(project, widgets)

# The widgets live in the watch app's PlugIns folder.
watch.add_dependency(widgets)
embed_widgets = watch.new_copy_files_build_phase('Embed Foundation Extensions')
embed_widgets.symbol_dst_subfolder_spec = :plug_ins
embed_widgets.add_file_reference(widgets.product_reference).settings = { 'ATTRIBUTES' => ['RemoveHeadersOnCopy'] }

# The watch app lives in the iOS app's Watch folder, and ships in its IPA.
app.add_dependency(watch)
embed_watch = app.new_copy_files_build_phase('Embed Watch Content')
embed_watch.dst_subfolder_spec = '16' # products directory
embed_watch.dst_path = '$(CONTENTS_FOLDER_PATH)/Watch'
embed_watch.add_file_reference(watch.product_reference).settings = { 'ATTRIBUTES' => ['RemoveHeadersOnCopy'] }
end

# --- Gate 2 extensions: widgets, share, AutoFill ----------------------------

# ForkMeshWidgets (WidgetKit and Live Activities, WS12), ForkMeshShare (the
# share extension, WS12) and ForkMeshAutoFill (the credential provider, WS21),
# embedded in the app beside NotificationService. Created after everything
# above so the ids of the existing objects stay put. Each links ForkMeshCore
# (the linker keeps only what it uses). None needs an App Group: what they
# share with the app lives in the keychain group the app already has.
# Each is added unless FORKMESH_NATIVE_WIDGETS=0 / FORKMESH_NATIVE_SHARE=0 /
# FORKMESH_NATIVE_AUTOFILL=0; FORKMESH_NATIVE_EXTENSIONS=1 forces all three.
surface_sources = synchronized_group(project, 'SharedSurfaces')
app.file_system_synchronized_groups << surface_sources
extension_targets = []
all_extensions = ENV.fetch('FORKMESH_NATIVE_EXTENSIONS', '0') == '1'
widgets_extension = all_extensions || ENV.fetch('FORKMESH_NATIVE_WIDGETS', '1') != '0'
share_extension = all_extensions || ENV.fetch('FORKMESH_NATIVE_SHARE', '1') != '0'
autofill_extension = all_extensions || ENV.fetch('FORKMESH_NATIVE_AUTOFILL', '1') != '0'
[
  ['ForkMeshWidgets', WIDGETS_BUNDLE_ID, widgets_extension],
  ['ForkMeshShare', SHARE_BUNDLE_ID, share_extension],
  ['ForkMeshAutoFill', AUTOFILL_BUNDLE_ID, autofill_extension],
].select { |_, _, wanted| wanted }.each do |name, bundle_id, _|
  %W[#{name}-Info.plist #{name}.entitlements].each { |file| version_config.new_file(file) }
  target = native_target(project, name, :app_extension, "#{name}.appex")
  target.file_system_synchronized_groups << synchronized_group(project, name)
  target.file_system_synchronized_groups << surface_sources if name == 'ForkMeshWidgets'
  configure(target, version_xcconfig) do |config|
    config.build_settings.merge!(
      'ASSETCATALOG_COMPILER_GENERATE_SWIFT_ASSET_SYMBOL_EXTENSIONS' => 'NO',
      'CODE_SIGN_ENTITLEMENTS' => "Config/#{name}.entitlements",
      'INFOPLIST_FILE' => "Config/#{name}-Info.plist",
      'LD_RUNPATH_SEARCH_PATHS' => [
        '$(inherited)', '@executable_path/Frameworks', '@executable_path/../../Frameworks'
      ],
      'PRODUCT_BUNDLE_IDENTIFIER' => bundle_id,
      'SKIP_INSTALL' => 'YES'
    )
  end
  product = project.new(Obj::XCSwiftPackageProductDependency)
  product.product_name = 'ForkMeshCore'
  target.package_product_dependencies << product
  build_file = project.new(Obj::PBXBuildFile)
  build_file.product_ref = product
  target.frameworks_build_phase.files << build_file
  app.add_dependency(target)
  embed.add_file_reference(target.product_reference).settings = { 'ATTRIBUTES' => ['RemoveHeadersOnCopy'] }
  extension_targets << target
end

project.save

# A shared scheme, so `xcodebuild -scheme ForkMesh` works on a fresh clone.
# It builds the watch app too, as the app's dependency.
scheme = Xcodeproj::XCScheme.new
scheme.add_build_target(app)
scheme.set_launch_target(app)
scheme.save_as(PROJECT_PATH, 'ForkMesh', true)

# The watch app on its own (`xcodebuild -scheme ForkMeshWatch`), widgets included.
# Skipped for a phone-only project: a scheme with no target crashes xcodebuild.
if watch
  watch_scheme = Xcodeproj::XCScheme.new
  watch_scheme.add_build_target(watch)
  watch_scheme.set_launch_target(watch)
  watch_scheme.save_as(PROJECT_PATH, 'ForkMeshWatch', true)
end

# The Swift package pins. Config/Package.resolved is their source (this
# script deletes the project folder first), copied to where Xcode reads it, so
# every build resolves MLX and its dependencies to the versions that last
# compiled rather than the newest in each range. To move a pin, update
# Config/Package.resolved: the "Metal toolchain and Swift packages" step on
# Codemagic prints every resolved version, and `git ls-remote --tags <url>`
# gives the revision.
workspace = File.join(PROJECT_PATH, 'project.xcworkspace')
FileUtils.mkdir_p(File.join(workspace, 'xcshareddata', 'swiftpm'))
File.write(File.join(workspace, 'contents.xcworkspacedata'), <<~XML)
  <?xml version="1.0" encoding="UTF-8"?>
  <Workspace
     version = "1.0">
     <FileRef
        location = "self:">
     </FileRef>
  </Workspace>
XML
FileUtils.cp(File.join(ROOT, 'Config', 'Package.resolved'),
             File.join(workspace, 'xcshareddata', 'swiftpm', 'Package.resolved'))

puts "Wrote #{PROJECT_PATH}"
