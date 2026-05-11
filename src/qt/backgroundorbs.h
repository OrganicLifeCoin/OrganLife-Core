// Copyright (c) 2023 The PIVX Core developers
// Copyright (c) 2026 The CTEAM Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_QT_BACKGROUNDORBS_H
#define PIVX_QT_BACKGROUNDORBS_H

#include <QTimer>
#include <QElapsedTimer>
#include <QVector2D>
#include <QDebug>
#include <QWidget>
#include <QPainter>

#if __has_include(<QOpenGLWidget>) && __has_include(<QOpenGLFunctions>) && __has_include(<QOpenGLShaderProgram>)
#define PIVX_HAS_QT_OPENGL_WIDGET 1
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#else
#define PIVX_HAS_QT_OPENGL_WIDGET 0
#endif

#if PIVX_HAS_QT_OPENGL_WIDGET
class BackgroundOrbsWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
public:
    explicit BackgroundOrbsWidget(QWidget* parent = nullptr) : QOpenGLWidget(parent) {
        timer.start();
        QTimer* t = new QTimer(this);
        connect(t, &QTimer::timeout, this, QOverload<>::of(&BackgroundOrbsWidget::update));
        t->start(16);
    }

    ~BackgroundOrbsWidget() {
        makeCurrent();
        delete program;
        doneCurrent();
    }

protected:
    void initializeGL() override {
        initializeOpenGLFunctions();
        glClearColor(0.1f, 0.0f, 0.2f, 1.0f);
        initShaders();
        qDebug() << "BackgroundOrbsWidget: initialized";
    }

    void resizeGL(int w, int h) override {
        glViewport(0, 0, w, h);
    }

    void paintGL() override {
        if (!program) return;
        glClear(GL_COLOR_BUFFER_BIT);
        program->bind();
        program->setUniformValue(u_time_loc, (float)timer.elapsed() / 1000.0f);
        program->setUniformValue(u_res_loc, QVector2D(width(), height()));
        static const float vertices[] = {-1.f,-1.f, 1.f,-1.f, -1.f,1.f, 1.f,1.f};
        program->enableAttributeArray("a_position");
        program->setAttributeArray("a_position", GL_FLOAT, vertices, 2);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        program->release();
    }

private:
    QOpenGLShaderProgram* program = nullptr;
    QElapsedTimer timer;
    int u_time_loc = -1;
    int u_res_loc = -1;

    void initShaders() {
        program = new QOpenGLShaderProgram();
        program->addShaderFromSourceCode(QOpenGLShader::Vertex,
            "#version 120\n"
            "attribute vec4 a_position;\n"
            "void main() { gl_Position = a_position; }");
        program->addShaderFromSourceCode(QOpenGLShader::Fragment,
            "#version 120\n"
            "uniform float u_time;\n"
            "uniform vec2 u_resolution;\n"
            "void main() {\n"
            "    vec2 uv = gl_FragCoord.xy / u_resolution.xy;\n"
            "    float aspect = u_resolution.x / u_resolution.y;\n"
            "    uv.x *= aspect;\n"
            "    vec3 color = vec3(0.04, 0.07, 0.13);\n"
            "    float t = u_time * 0.15;\n"
            "    vec2 p1 = vec2(0.5*aspect+0.4*cos(t*0.8), 0.5+0.3*sin(t*1.1));\n"
            "    color += vec3(0.15,0.05,0.4)*(0.2/(length(uv-p1)*length(uv-p1)+0.2));\n"
            "    vec2 p2 = vec2(0.3*aspect+0.3*sin(t*1.3), 0.7+0.2*cos(t*0.9));\n"
            "    color += vec3(0.05,0.15,0.5)*(0.15/(length(uv-p2)*length(uv-p2)+0.15));\n"
            "    vec2 p3 = vec2(0.8*aspect+0.2*cos(t*1.5), 0.3+0.4*sin(t*0.7));\n"
            "    color += vec3(0.0,0.2,0.3)*(0.18/(length(uv-p3)*length(uv-p3)+0.18));\n"
            "    gl_FragColor = vec4(color, 1.0);\n"
            "}");
        program->link();
        u_time_loc = program->uniformLocation("u_time");
        u_res_loc = program->uniformLocation("u_resolution");
    }
};
#else
class BackgroundOrbsWidget : public QWidget
{
public:
    explicit BackgroundOrbsWidget(QWidget* parent = nullptr) : QWidget(parent) {
        timer.start();
        QTimer* t = new QTimer(this);
        connect(t, &QTimer::timeout, this, QOverload<>::of(&BackgroundOrbsWidget::update));
        t->start(16);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(10, 18, 33));
    }

private:
    QElapsedTimer timer;
};
#endif

#endif // PIVX_QT_BACKGROUNDORBS_H
