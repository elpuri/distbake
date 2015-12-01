import QtQuick 2.0

Item {
    width: 700
    height: 700
    focus: true
    Keys.onPressed: {
        if (event.key === Qt.Key_Z)
            scaled.scaling = scaled.scaling * 0.80;
        else if (event.key === Qt.Key_X)
            scaled.scaling = scaled.scaling * 1.20;
        else if (event.key === Qt.Key_Escape)
            Qt.quit()
    }

    Flickable {
        anchors.fill: parent
        contentWidth: scaled.width
        contentHeight: scaled.height

        ShaderEffect {
            id: scaled
            property real scaling : 1.0
            width: distanceField.width * scaling
            height: distanceField.height * scaling
            property variant df: distanceField
            fragmentShader: "
                 uniform sampler2D df;
                 varying highp vec2 qt_TexCoord0;

                 void main() {
                      highp float sharpness = dFdx(qt_TexCoord0.s) * 80.0;
                      sharpness = min(sharpness, 0.5);
                      highp float d = texture2D(df, qt_TexCoord0).r;
                      lowp vec3 color = vec3(smoothstep(0.5 + sharpness, 0.5 - sharpness, d));
                      gl_FragColor = vec4(color, 1.0);
                 }
            "
        }
    }

    Image {
        anchors.centerIn: parent
        id: distanceField
        visible: false
        source: "distancefield.png"
    }
}

