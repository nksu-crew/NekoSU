# Add project specific ProGuard rules here.
# You can control the set of applied configuration files using the
# proguardFiles setting in build.gradle.
#
# For more details, see
#   http://developer.android.com/guide/developing/tools/proguard.html

# If your project uses WebView with JS, uncomment the following
# and specify the fully qualified class name to the JavaScript interface
# class:
#-keepclassmembers class fqcn.of.javascript.interface.for.webview {
#   public *;
#}

# Uncomment this to preserve the line number information for
# debugging stack traces.
#-keepattributes SourceFile,LineNumberTable

# If you keep the line number information, uncomment this to
# hide the original source file name.
#-renamesourcefileattribute SourceFile

# Keep OkHttp3 classes
-dontwarn okhttp3.**
-keep class okhttp3.** { *; }
-keep interface okhttp3.** { *; }

-keep class me.nekosu.aqnya.util.GitHubRelease { 
    *; 
}

-keep class me.nekosu.aqnya.ncore { *; }

# WebUI: the module page calls `window.ksu.*` through the WebView JavaBridge,
# which resolves the annotated methods reflectively.  Keep them from R8.
-keepclassmembers class * {
    @android.webkit.JavascriptInterface <methods>;
}
-keep class me.nekosu.aqnya.ui.webui.WebViewBridge { *; }

-keepattributes RuntimeVisibleAnnotations, RuntimeVisibleParameterAnnotations
-keepattributes AnnotationDefault, Signature, InnerClasses, EnclosingMethod
