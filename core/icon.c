#include "icon.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "mesh.h"

#define CENTRE 16 // the client centres icons here, not at the middle of the canvas

static int sinTable[2048], cosTable[2048], tablesReady = 0;
static int canvasW = ICON_W, canvasH = ICON_H; // heads use a bigger one than items

static void prepare_tables(void) {
	if (tablesReady) return;
	for (int i = 0; i < 2048; i++) {
		sinTable[i] = (int)(sin(i * 0.0030679615) * 65536.0);
		cosTable[i] = (int)(cos(i * 0.0030679615) * 65536.0);
	}
	tablesReady = 1;
}

typedef struct {
	int x, y, depth;
	int r, g, b;
} Corner;

static int edge(int ax, int ay, int bx, int by, int px, int py) {
	return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static void fill_triangle(const Corner* a, const Corner* b, const Corner* c, uint32_t* pixels, int* depths) {
	int area = edge(a->x, a->y, b->x, b->y, c->x, c->y);
	if (area == 0) return;
	int minX = a->x < b->x ? a->x : b->x, maxX = a->x > b->x ? a->x : b->x;
	int minY = a->y < b->y ? a->y : b->y, maxY = a->y > b->y ? a->y : b->y;
	if (c->x < minX) minX = c->x;
	if (c->x > maxX) maxX = c->x;
	if (c->y < minY) minY = c->y;
	if (c->y > maxY) maxY = c->y;
	if (minX < 0) minX = 0;
	if (minY < 0) minY = 0;
	if (maxX > canvasW - 1) maxX = canvasW - 1;
	if (maxY > canvasH - 1) maxY = canvasH - 1;
	for (int y = minY; y <= maxY; y++) {
		for (int x = minX; x <= maxX; x++) {
			int w0 = edge(b->x, b->y, c->x, c->y, x, y), w1 = edge(c->x, c->y, a->x, a->y, x, y), w2 = edge(a->x, a->y, b->x, b->y, x, y);
			if (area > 0 ? (w0 < 0 || w1 < 0 || w2 < 0) : (w0 > 0 || w1 > 0 || w2 > 0)) continue;
			long depth = ((long)w0 * a->depth + (long)w1 * b->depth + (long)w2 * c->depth) / area;
			int at = y * canvasW + x;
			if (depth >= depths[at]) continue;
			depths[at] = (int)depth;
			int r = (int)(((long)w0 * a->r + (long)w1 * b->r + (long)w2 * c->r) / area);
			int g = (int)(((long)w0 * a->g + (long)w1 * b->g + (long)w2 * c->g) / area);
			int bl = (int)(((long)w0 * a->b + (long)w1 * b->b + (long)w2 * c->b) / area);
			uint32_t colour = (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)bl;
			pixels[at] = colour ? colour : 1; // 0 means transparent
		}
	}
}

static void outline(uint32_t* pixels, uint32_t colour) {
	static uint32_t out[ICON_W * ICON_H];
	for (int y = 0, at = 0; y < ICON_H; y++) {
		for (int x = 0; x < ICON_W; x++, at++) {
			uint32_t p = pixels[at];
			if (p == 0 && ((x > 0 && pixels[at - 1]) || (y > 0 && pixels[at - ICON_W]) || (x < ICON_W - 1 && pixels[at + 1]) || (y < ICON_H - 1 && pixels[at + ICON_W]))) p = colour;
			out[at] = p;
		}
	}
	memcpy(pixels, out, sizeof(out));
}

static void shadow(uint32_t* pixels, uint32_t colour) {
	for (int y = ICON_H - 1; y > 0; y--)
		for (int x = ICON_W - 1; x > 0; x--)
			if (pixels[y * ICON_W + x] == 0 && pixels[(y - 1) * ICON_W + x - 1] != 0) pixels[y * ICON_W + x] = colour;
}

static const ObjDef* stack_variant(Scene* s, const ObjDef* d, int stack) {
	if (stack <= 1 || !d->hasCounts) return d;
	int id = -1;
	for (int i = 0; i < 10; i++) if (stack >= d->countCo[i] && d->countCo[i] != 0) id = d->countObj[i];
	const ObjDef* other = id == -1 ? NULL : scene_get_obj(s, id);
	return other ? other : d;
}

// The model on its own, without the outline or anything drawn over it.
static int draw_model(Scene* s, const uint32_t* palette, const ObjDef* d, int zoom, uint32_t* pixels) {
	SceneBase* m = scene_build_item(s, d);
	if (!m) return -1;
	size_t triangles = (size_t)m->triangleCount * 3;
	uint16_t* cornerVertex = malloc(triangles * 2 + 2);
	uint16_t* indices = malloc(triangles * 2 + 2);
	uint8_t* colours = malloc(triangles * 4 + 4);
	int* sx = malloc(sizeof(int) * (size_t)(m->vertexCount + 1));
	int* sy = malloc(sizeof(int) * (size_t)(m->vertexCount + 1));
	int* sd = malloc(sizeof(int) * (size_t)(m->vertexCount + 1));
	static int depths[ICON_W * ICON_H];
	uint32_t indexCount = 0;
	int rc = -1;
	if (cornerVertex && indices && colours && sx && sy && sd && mesh_index_model(s, palette, m, cornerVertex, colours, indices, &indexCount) >= 0) {
		int minY = 0;
		for (int v = 0; v < m->vertexCount; v++) if (m->vy[v] < minY) minY = m->vy[v];
		int pitch = d->xan2d & 2047, yaw = d->yan2d & 2047, roll = d->zan2d & 2047;
		int pitchCos = cosTable[pitch] * zoom >> 16, pitchSin = sinTable[pitch] * zoom >> 16;
		int offsetX = d->xof2d, offsetY = pitchSin + d->yof2d - minY / 2, offsetZ = d->yof2d + pitchCos;
		int sinPitch = sinTable[pitch], cosPitch = cosTable[pitch], sinYaw = sinTable[yaw], cosYaw = cosTable[yaw], sinRoll = sinTable[roll], cosRoll = cosTable[roll];
		int cameraDistance = (offsetY * sinPitch + offsetZ * cosPitch) >> 16;
		for (int v = 0; v < m->vertexCount; v++) {
			int x = m->vx[v], y = m->vy[v], z = m->vz[v], temp;
			if (roll != 0) {
				temp = (y * sinRoll + x * cosRoll) >> 16;
				y = (y * cosRoll - x * sinRoll) >> 16;
				x = temp;
			}
			if (yaw != 0) {
				temp = (z * sinYaw + x * cosYaw) >> 16;
				z = (z * cosYaw - x * sinYaw) >> 16;
				x = temp;
			}
			x += offsetX;
			y += offsetY;
			z += offsetZ;
			temp = (y * cosPitch - z * sinPitch) >> 16;
			z = (y * sinPitch + z * cosPitch) >> 16;
			if (z == 0) z = 1;
			sd[v] = z - cameraDistance;
			sx[v] = CENTRE + x * 512 / z;
			sy[v] = CENTRE + temp * 512 / z;
		}
		for (int i = 0; i < ICON_W * ICON_H; i++) depths[i] = INT_MAX;
		for (uint32_t i = 0; i + 2 < indexCount; i += 3) {
			Corner t[3];
			int transparent = 0;
			for (int k = 0; k < 3; k++) {
				int corner = indices[i + (uint32_t)k], v = cornerVertex[corner];
				const uint8_t* c = colours + corner * 4;
				t[k] = (Corner){sx[v], sy[v], sd[v], c[0], c[1], c[2]};
				if (c[3] == 0) transparent = 1;
			}
			// Faces turned away from the camera are not drawn.
			if (transparent || (t[0].x - t[1].x) * (t[2].y - t[1].y) - (t[0].y - t[1].y) * (t[2].x - t[1].x) <= 0) continue;
			fill_triangle(&t[0], &t[1], &t[2], pixels, depths);
		}
		rc = 0;
	}
	free(cornerVertex);
	free(indices);
	free(colours);
	free(sx);
	free(sy);
	free(sd);
	scene_free_base(m);
	return rc;
}

static void overlay(uint32_t* under, const uint32_t* over) {
	for (int i = 0; i < ICON_W * ICON_H; i++) if (over[i]) under[i] = over[i];
}

static int render(Scene* s, const uint32_t* palette, int id, int stack, int state, uint32_t shadowColour, int noteInner, uint32_t* pixels) {
	const ObjDef* d = scene_get_obj(s, id);
	if (!d) return -1;
	d = stack_variant(s, d, stack);
	static uint32_t linked[ICON_W * ICON_H];
	int hasLinked = 0;
	if (d->certTemplate != -1) {
		memset(linked, 0, sizeof(linked));
		if (render(s, palette, d->certLink, 10, 1, 0, 1, linked) != 0) return -1;
		hasLinked = 1;
	} else if (d->lentTemplate != -1) {
		memset(linked, 0, sizeof(linked));
		if (render(s, palette, d->lentLink, stack, state, shadowColour, 0, linked) != 0) return -1;
		hasLinked = 2;
	}
	uint32_t own[ICON_W * ICON_H];
	memset(own, 0, sizeof(own));
	int zoom = d->zoom2d;
	if (noteInner) zoom = (int)(zoom * 1.5);
	else if (state == 2) zoom = (int)(zoom * 1.04);
	if (draw_model(s, palette, d, zoom, own) != 0) return -1;
	if (state >= 1) outline(own, 1);
	if (state >= 2) outline(own, 0xFFFFFF);
	if (shadowColour) shadow(own, shadowColour);
	if (hasLinked == 1) overlay(own, linked);
	if (hasLinked == 2) {
		overlay(linked, own);
		memcpy(own, linked, sizeof(own));
	}
	memcpy(pixels, own, sizeof(own));
	return 0;
}

int icon_render(Scene* s, const uint32_t* palette, int id, int stack, uint32_t* pixels) {
	prepare_tables();
	return render(s, palette, id, stack, 1, ICON_SHADOW, 0, pixels);
}

#define HEAD_MAX 96
#define HEAD_YAW 128 // turned a little, as heads are in the original

static int headYaw = HEAD_YAW;

void head_set_yaw(int yaw) {
	headYaw = yaw < 0 ? HEAD_YAW : yaw & 2047;
}

int head_prepare(Scene* s, const uint32_t* palette, const SceneBase* m, HeadMesh* out) {
	memset(out, 0, sizeof(*out));
	size_t corners = (size_t)m->triangleCount * 3;
	out->cornerVertex = malloc(corners * 2 + 2);
	out->indices = malloc(corners * 2 + 2);
	out->colours = malloc(corners * 4 + 4);
	if (!out->cornerVertex || !out->indices || !out->colours || mesh_index_model(s, palette, m, out->cornerVertex, out->colours, out->indices, &out->indexCount) < 0) {
		head_free(out);
		return -1;
	}
	int minX = INT_MAX, maxX = INT_MIN, minY = INT_MAX, maxY = INT_MIN;
	for (int v = 0; v < m->vertexCount; v++) {
		if (m->vx[v] < minX) minX = m->vx[v];
		if (m->vx[v] > maxX) maxX = m->vx[v];
		if (m->vy[v] < minY) minY = m->vy[v];
		if (m->vy[v] > maxY) maxY = m->vy[v];
	}
	int span = maxX - minX > maxY - minY ? maxX - minX : maxY - minY;
	out->centreX = (minX + maxX) / 2;
	out->centreY = (minY + maxY) / 2;
	out->scale = span > 0 ? 256 * 256 / span : 256; // turned into pixels when the size is known
	return 0;
}

void head_free(HeadMesh* mesh) {
	free(mesh->cornerVertex);
	free(mesh->indices);
	free(mesh->colours);
	memset(mesh, 0, sizeof(*mesh));
}

void head_draw(const HeadMesh* mesh, const int32_t* vx, const int32_t* vy, const int32_t* vz, int vertexCount, uint32_t* pixels, int size) {
	static int depths[HEAD_MAX * HEAD_MAX];
	if (size > HEAD_MAX) return;
	prepare_tables();
	int* sx = malloc(sizeof(int) * (size_t)(vertexCount + 1));
	int* sy = malloc(sizeof(int) * (size_t)(vertexCount + 1));
	int* sd = malloc(sizeof(int) * (size_t)(vertexCount + 1));
	if (sx && sy && sd) {
		int fit = mesh->scale * (size - 6) / 256; // 256ths of a pixel per model unit
		int sinYaw = sinTable[headYaw], cosYaw = cosTable[headYaw];
		for (int v = 0; v < vertexCount; v++) {
			int x = vx[v] - mesh->centreX, y = vy[v] - mesh->centreY, z = vz[v];
			int turned = (z * sinYaw + x * cosYaw) >> 16;
			z = (z * cosYaw - x * sinYaw) >> 16;
			sx[v] = size / 2 + (turned * fit >> 8);
			sy[v] = size / 2 + (y * fit >> 8);
			sd[v] = z;
		}
		for (int i = 0; i < size * size; i++) depths[i] = INT_MAX;
		canvasW = canvasH = size;
		for (uint32_t i = 0; i + 2 < mesh->indexCount; i += 3) {
			Corner t[3];
			int transparent = 0;
			for (int k = 0; k < 3; k++) {
				int corner = mesh->indices[i + (uint32_t)k], v = mesh->cornerVertex[corner];
				const uint8_t* c = mesh->colours + corner * 4;
				t[k] = (Corner){sx[v], sy[v], sd[v], c[0], c[1], c[2]};
				if (c[3] == 0) transparent = 1;
			}
			if (transparent || (t[0].x - t[1].x) * (t[2].y - t[1].y) - (t[0].y - t[1].y) * (t[2].x - t[1].x) <= 0) continue;
			fill_triangle(&t[0], &t[1], &t[2], pixels, depths);
		}
		canvasW = ICON_W;
		canvasH = ICON_H;
	}
	free(sx);
	free(sy);
	free(sd);
}
