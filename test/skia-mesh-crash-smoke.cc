// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include <cstdio>
#include <cstdlib>

#include "include/core/SkBBHFactory.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkMesh.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"

namespace {
void Require(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "%s\n", message);
    std::abort();
  }
}

class MeshConsumer final : public SkCanvas {
 public:
  MeshConsumer() : SkCanvas(64, 64) {}
  int draws = 0;

 protected:
  void onDrawMesh(const SkMesh& mesh, sk_sp<SkBlender>, const SkPaint& paint) override {
    Require(mesh.isValid() && mesh.vertexCount() == 3, "Recorded mesh lost its vertices");
    Require(mesh.bounds() == SkRect::MakeLTRB(4, 4, 24, 24), "Recorded mesh bounds changed");
    Require(paint.getColor() == SK_ColorRED, "Recorded mesh paint changed");
    ++draws;
  }
};
}  // namespace

int main() {
  sk_sp<SkPicture> picture;
  {
    const SkMeshSpecification::Attribute attribute{
        SkMeshSpecification::Attribute::Type::kFloat2, 0, SkString("position")};
    auto specification = SkMeshSpecification::Make(
        {&attribute, 1}, sizeof(SkPoint), {},
        SkString("Varyings main(const Attributes a) { Varyings v; v.position = a.position; return v; }"),
        SkString("float2 main(const Varyings v) { return v.position; }"));
    Require(bool(specification.specification), specification.error.c_str());
    const SkPoint vertices[] = {{4, 4}, {24, 4}, {4, 24}};
    auto buffer = SkMeshes::MakeVertexBuffer(vertices, sizeof(vertices));
    auto mesh = SkMesh::Make(specification.specification, SkMesh::Mode::kTriangles,
                            buffer, 3, 0, nullptr, {}, SkRect::MakeLTRB(4, 4, 24, 24));
    Require(mesh.mesh.isValid(), mesh.error.c_str());
    SkRTreeFactory hierarchy;
    SkPictureRecorder recorder;
    SkPaint paint;
    paint.setColor(SK_ColorRED);
    recorder.beginRecording(64, 64, &hierarchy)->drawMesh(mesh.mesh, nullptr, paint);
    picture = recorder.finishRecordingAsPicture();
  }
  // Original mesh, specification, vertex buffer and recorder have all died.
  MeshConsumer consumer;
  picture->playback(&consumer);
  Require(consumer.draws == 1, "Recording did not retain and replay the mesh");
  consumer.save();
  consumer.clipRect(SkRect::MakeLTRB(40, 40, 60, 60));
  picture->playback(&consumer);
  consumer.restore();
  Require(consumer.draws == 1, "Recorded mesh bounds failed to cull an outside clip");
  std::puts("PASS: Skia mesh recording retains resources, replays paint and honors culling");
  return 0;
}
