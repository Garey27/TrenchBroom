/*
 Copyright (C) 2010-2012 Kristian Duske

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "Map.h"
#include <algorithm>
#include <fstream>
#include <cassert>
#include "Model/Map/Brush.h"
#include "Model/Map/Entity.h"
#include "Model/Map/EntityDefinition.h"
#include "Model/Map/Face.h"
#include "Model/Map/Groups.h"
#include "Model/Map/Picker.h"
#include "Model/Octree.h"
#include "Model/Selection.h"
#include "Utilities/Utils.h"

namespace TrenchBroom {
    namespace Model {
        void Map::setPostNotifications(bool postNotifications) {
            m_postNotifications = postNotifications;
        }

        Map::Map(const BBox& worldBounds, const string& entityDefinitionFilePath) : m_worldBounds(worldBounds), m_worldspawn(NULL) {
            m_octree = new Octree(*this, 256);
            m_picker = new Picker(*m_octree);
            m_selection = new Selection();
            m_entityDefinitionManager = EntityDefinitionManager::sharedManager(entityDefinitionFilePath);
            m_groupManager = new GroupManager(*this);
            m_postNotifications = true;

            m_mods.push_back("id1");
        }

        Map::~Map() {
            setPostNotifications(false);
            clear();
            delete m_octree;
            delete m_picker;
            delete m_selection;
            delete m_groupManager;
        }

        void Map::save(const string& path) {
        }

        void Map::clear() {
            m_selection->removeAll();
            unloadPointFile();
            while(!m_entities.empty()) delete m_entities.back(), m_entities.pop_back();
            m_worldspawn = NULL;
            if (m_postNotifications) mapCleared(*this);
        }

# pragma mark Point File Support

        void Map::loadPointFile(const string& path) {
            if (!m_leakPoints.empty()) unloadPointFile();

            string line;
            ifstream stream(path.c_str());
            assert(stream.is_open());

            while (!stream.eof()) {
                getline(stream, line);
                line = trim(line);
                if (line.length() > 0) {
                    Vec3f point = Vec3f(line);
                    m_leakPoints.push_back(point);
                }
            }

            if (m_postNotifications) pointFileLoaded(*this);
        }

        void Map::unloadPointFile() {
            m_leakPoints.clear();
            if (m_postNotifications) pointFileUnloaded(*this);

        }

        const vector<Vec3f>& Map::leakPoints() const {
            return m_leakPoints;
        }

# pragma mark Entity related functions
        vector<Entity*>& Map::entities() {
            return m_entities;
        }

        Entity* Map::worldspawn(bool create) {
            if (m_worldspawn != NULL)
                return m_worldspawn;
            for (int i = 0; i < m_entities.size(); i++) {
                Entity* entity = m_entities[i];
                if (entity->worldspawn()) {
                    m_worldspawn = entity;
                    return m_worldspawn;
                }
            }

            if (create)
                m_worldspawn = createEntity(WorldspawnClassname);
            return m_worldspawn;
        }

        void Map::addEntity(Entity* entity) {
            assert(entity != NULL);
            if (!entity->worldspawn() || worldspawn(false) == NULL) {
                m_entities.push_back(entity);
                entity->setMap(this);
                setEntityDefinition(entity);
            }

            vector <Entity*> entities;
            entities.push_back(entity);
            if (m_postNotifications) entitiesWereAdded(entities);
        }

        Entity* Map::createEntity(const string& classname) {
            Entity* entity = new Entity();
            entity->setProperty(ClassnameKey, classname);
            addEntity(entity);
            return entity;
        }

        Entity* Map::createEntity(const map<string, string> properties) {
            Entity* entity = new Entity(properties);
            addEntity(entity);
            return entity;
        }

        void Map::setEntityDefinition(Entity* entity) {
            const string* classname = entity->classname();
            if (classname != NULL) {
                EntityDefinition* entityDefinition = m_entityDefinitionManager->definition(*classname);
                if (entityDefinition != NULL)
                    entity->setEntityDefinition(entityDefinition);
                // else
                //    fprintf(stdout, "Warning: No entity definition found for class name '%s'\n", classname->c_str());
            } else {
                fprintf(stdout, "Warning: Entity with id %i is missing classname property (line %i)\n", entity->uniqueId(), entity->filePosition());
            }
        }

        void Map::setEntityProperty(const string& key, const string* value) {
            const vector<Entity*>& entities = m_selection->entities();
            if (entities.empty()) return;

            vector<Entity*> changedEntities;
            for (int i = 0; i < entities.size(); i++) {
                Entity* entity = entities[i];
                const string* oldValue = entity->propertyForKey(key);
                if (oldValue != value) changedEntities.push_back(entity);
            }

            if (!changedEntities.empty()) {
                if (m_postNotifications) propertiesWillChange(changedEntities);
                for (int i = 0; i < changedEntities.size(); i++) {
                    if (value == NULL) entities[i]->deleteProperty(key);
                    else entities[i]->setProperty(key, value);
                }
                if (m_postNotifications) propertiesDidChange(changedEntities);
            }
        }

# pragma mark Brush related functions

        void Map::addBrushesToEntity(Entity& entity) {
            const vector<Brush*>& brushes = m_selection->brushes();
            if (brushes.empty()) return;

            entity.addBrushes(brushes);
            if (m_postNotifications) brushesWereAdded(brushes);
        }

        void Map::moveBrushesToEntity(Entity& entity) {
            const vector<Brush*> brushes = m_selection->brushes();
            if (brushes.empty()) return;

            if (m_postNotifications) brushesWillChange(brushes);
            entity.addBrushes(brushes);
            if (m_postNotifications) brushesDidChange(brushes);
        }

        Brush* Map::createBrush(Entity& entity, Brush& brushTemplate) {
            BBox templateBounds = brushTemplate.bounds();
            if (!m_worldBounds.contains(brushTemplate.bounds())) return NULL;

            Brush* brush = new Brush(m_worldBounds, brushTemplate);
            m_selection->removeAll();
            m_selection->addBrush(*brush);
            addBrushesToEntity(entity);
            return brush;
        }

        Brush* Map::createBrush(Entity& entity, BBox bounds, Assets::Texture& texture) {
            if (!m_worldBounds.contains(bounds)) return NULL;

            Brush* brush = new Brush(m_worldBounds, bounds, texture);
            m_selection->removeAll();
            m_selection->addBrush(*brush);
            addBrushesToEntity(entity);
            return brush;
        }

        void Map::snapBrushes() {
            const vector<Brush*>& brushes = m_selection->brushes();
            if (brushes.empty()) return;

            if (m_postNotifications) brushesWillChange(brushes);
            for (int i = 0; i < brushes.size(); i++)
                brushes[i]->snap();
            if (m_postNotifications) brushesDidChange(brushes);
        }

        bool Map::resizeBrushes(vector<Face*>& faces, float delta, bool lockTextures) {
            if (faces.empty()) return false;
            if (delta == 0) return false;

            bool drag = true;
            vector<Brush*> changedBrushes;
            for (int i = 0; i < faces.size() && drag; i++) {
                Face* face = faces[i];
                Brush* brush = face->brush();
                drag &= brush->selected() && brush->canResize(*face, delta);
                changedBrushes.push_back(brush);
            }

            if (drag) {
                if (m_postNotifications) brushesWillChange(changedBrushes);
                for (int i = 0; i < faces.size(); i++) {
                    Face* face = faces[i];
                    Brush* brush = face->brush();
                    brush->resize(*face, delta, lockTextures);
                }
                if (m_postNotifications) brushesDidChange(changedBrushes);
            }

            return drag;
        }

# pragma mark Common functions

        void Map::duplicateObjects(vector<Entity*>& newEntities, vector<Brush*>& newBrushes) {
            const vector<Entity*>& entities = m_selection->entities();
            const vector<Brush*>& brushes = m_selection->brushes();

            if (!entities.empty()) {
                for (int i = 0; i < entities.size(); i++) {
                    Entity* entity = entities[i];
                    Entity* newEntity = new Entity(entity->properties());

                    EntityDefinition* entityDefinition = m_entityDefinitionManager->definition(*newEntity->classname());
                    assert(entityDefinition != NULL);
                    newEntity->setEntityDefinition(entityDefinition);

                    newEntities.push_back(newEntity);
                    m_entities.push_back(newEntity);

                    for (int i = 0; i < entity->brushes().size(); i++) {
                        Brush* newBrush = new Brush(m_worldBounds, *entity->brushes()[i]);
                        newBrushes.push_back(newBrush);
                        newEntity->addBrush(newBrush);
                    }
                }
            }

            if (!brushes.empty()) {
                for (int i = 0; i < brushes.size(); i++) {
                    Brush* newBrush = new Brush(m_worldBounds, *brushes[i]);
                    newBrushes.push_back(newBrush);
                    worldspawn(true)->addBrush(newBrush);
                }
            }

            if (!newEntities.empty() && m_postNotifications) entitiesWereAdded(newEntities);
            if (!newBrushes.empty() && m_postNotifications) brushesWereAdded(newBrushes);
        }

        void Map::translateObjects(const Vec3f& delta, bool lockTextures) {
            const vector<Entity*>& entities = m_selection->entities();
            const vector<Brush*>& brushes = m_selection->brushes();

            if (!entities.empty()) {
                if (m_postNotifications) propertiesWillChange(entities);
                for (int i = 0; i < entities.size(); i++)
                    entities[i]->translate(delta);
                if (m_postNotifications) propertiesDidChange(entities);
            }

            if (!brushes.empty()) {
                if (m_postNotifications) brushesWillChange(brushes);
                for (int i = 0; i < brushes.size(); i++)
                    brushes[i]->translate(delta, lockTextures);
                if (m_postNotifications) brushesDidChange(brushes);
            }
        }

        void Map::rotateObjects90(EAxis axis, const Vec3f& center, bool clockwise, bool lockTextures) {
            const vector<Entity*>& entities = m_selection->entities();
            const vector<Brush*>& brushes = m_selection->brushes();

            if (!entities.empty()) {
                if (m_postNotifications) propertiesWillChange(entities);
                for (int i = 0; i < entities.size(); i++)
                    entities[i]->rotate90(axis, center, clockwise);
                if (m_postNotifications) propertiesDidChange(entities);
            }

            if (!brushes.empty()) {
                if (m_postNotifications) brushesWillChange(brushes);
                for (int i = 0; i < brushes.size(); i++)
                    brushes[i]->rotate90(axis, center, clockwise, lockTextures);
                if (m_postNotifications) brushesDidChange(brushes);
            }
        }

        void Map::rotateObjects(const Quat& rotation, const Vec3f& center, bool lockTextures) {
            const vector<Entity*>& entities = m_selection->entities();
            const vector<Brush*>& brushes = m_selection->brushes();

            if (!entities.empty()) {
                if (m_postNotifications) propertiesWillChange(entities);
                for (int i = 0; i < entities.size(); i++)
                    entities[i]->rotate(rotation, center);
                if (m_postNotifications) propertiesDidChange(entities);
            }

            if (!brushes.empty()) {
                if (m_postNotifications) brushesWillChange(brushes);
                for (int i = 0; i < brushes.size(); i++)
                    brushes[i]->rotate(rotation, center, lockTextures);
                if (m_postNotifications) brushesDidChange(brushes);
            }
        }

        void Map::flipObjects(EAxis axis, const Vec3f& center, bool lockTextures) {
            const vector<Entity*>& entities = m_selection->entities();
            const vector<Brush*>& brushes = m_selection->brushes();

            if (!entities.empty()) {
                if (m_postNotifications) propertiesWillChange(entities);
                for (int i = 0; i < entities.size(); i++)
                    entities[i]->flip(axis, center);
                if (m_postNotifications) propertiesDidChange(entities);
            }

            if (!brushes.empty()) {
                if (m_postNotifications) brushesWillChange(brushes);
                for (int i = 0; i < brushes.size(); i++)
                    brushes[i]->flip(axis, center, lockTextures);
                if (m_postNotifications) brushesDidChange(brushes);
            }
        }

        void Map::deleteObjects() {
            const vector<Entity*>& entities = m_selection->entities();
            const vector<Brush*>& brushes = m_selection->brushes();

            vector<Entity*> removedEntities;
            if (!brushes.empty()) {
                vector<Brush*> removedBrushes = brushes;
                if (m_postNotifications) brushesWillBeRemoved(removedBrushes);
                m_selection->removeBrushes(removedBrushes);
                for (int i = 0; i < removedBrushes.size(); i++) {
                    Brush* brush = removedBrushes[i];
                    Entity* entity = brush->entity();
                    entity->removeBrush(brush);
                    delete brush;

                    if (entity->brushes().empty() && !entity->worldspawn())
                        removedEntities.push_back(entity);
                }
            }

            if (!removedEntities.empty() || !entities.empty()) {
                for (int i = 0; i < entities.size(); i++) {
                    Entity* entity = entities[i];
                    if (!entity->worldspawn()) {
                        worldspawn(true)->addBrushes(entity->brushes());
                        if (find(removedEntities.begin(), removedEntities.end(), entity) == removedEntities.end())
                            removedEntities.push_back(entity);
                    }
                }

                if (m_postNotifications) entitiesWillBeRemoved(removedEntities);
                m_selection->removeEntities(removedEntities);
                for (int i = 0; i < removedEntities.size(); i++) {
                    remove(m_entities.begin(), m_entities.end(), removedEntities[i]);
                    delete removedEntities[i];
                }
            }
        }

# pragma mark Face related functoins
        void Map::setXOffset(int xOffset) {
            const vector<Face*>& faces = m_selection->faces();
            if (faces.empty()) return;

            if (m_postNotifications) facesWillChange(faces);
            for (int i = 0; i < faces.size(); i++)
                faces[i]->setXOffset(xOffset);
            if (m_postNotifications) facesDidChange(faces);
        }

        void Map::setYOffset(int yOffset) {
            const vector<Face*>& faces = m_selection->faces();
            if (faces.empty()) return;

            if (m_postNotifications) facesWillChange(faces);
            for (int i = 0; i < faces.size(); i++)
                faces[i]->setYOffset(yOffset);
            if (m_postNotifications) facesDidChange(faces);
        }

        void Map::translateFaces(float delta, const Vec3f& dir) {
            const vector<Face*>& faces = m_selection->faces();
            if (faces.empty()) return;

            if (m_postNotifications) facesWillChange(faces);
            for (int i = 0; i < faces.size(); i++)
                faces[i]->translateOffsets(delta, dir);
            if (m_postNotifications) facesDidChange(faces);
        }

        void Map::setRotation(float rotation) {
            const vector<Face*>& faces = m_selection->faces();
            if (faces.empty()) return;

            if (m_postNotifications) facesWillChange(faces);
            for (int i = 0; i < faces.size(); i++)
                faces[i]->setRotation(rotation);
            if (m_postNotifications) facesDidChange(faces);
        }

        void Map::rotateFaces(float angle) {
            const vector<Face*>& faces = m_selection->faces();
            if (faces.empty()) return;

            if (m_postNotifications) facesWillChange(faces);
            for (int i = 0; i < faces.size(); i++)
                faces[i]->rotateTexture(angle);
            if (m_postNotifications) facesDidChange(faces);
        }

        void Map::setXScale(float xScale) {
            const vector<Face*>& faces = m_selection->faces();
            if (faces.empty()) return;

            if (m_postNotifications) facesWillChange(faces);
            for (int i = 0; i < faces.size(); i++)
                faces[i]->setXScale(xScale);
            if (m_postNotifications) facesDidChange(faces);
        }

        void Map::setYScale(float yScale) {
            const vector<Face*>& faces = m_selection->faces();
            if (faces.empty()) return;

            if (m_postNotifications) facesWillChange(faces);
            for (int i = 0; i < faces.size(); i++)
                faces[i]->setYScale(yScale);
            if (m_postNotifications) facesDidChange(faces);
        }

        bool Map::deleteFaces() {
            const vector<Face*> faces = m_selection->faces();
            if (faces.empty()) return false;

            vector<Brush*> changedBrushes;
            bool del = true;
            for (int i = 0; i < faces.size() && del; i++) {
                Face* face = faces[i];
                Brush* brush = face->brush();
                del &= brush->canDeleteFace(*face);
                changedBrushes.push_back(brush);
            }

            if (del) {
                m_selection->removeAll();
                m_selection->addBrushes(changedBrushes);
                if (m_postNotifications) brushesWillChange(changedBrushes);
                for (int i = 0; i < faces.size() && del; i++) {
                    Face* face = faces[i];
                    Brush* brush = face->brush();
                    brush->deleteFace(*face);
                }
                if (m_postNotifications) brushesDidChange(changedBrushes);
            }

            return del;
        }

# pragma mark Vertex related functions
        MoveResult Map::moveVertex(Brush& brush, int vertexIndex, const Vec3f& delta) {
            if (find(m_selection->brushes().begin(), m_selection->brushes().end(), &brush) == m_selection->brushes().end())
                m_selection->addBrush(brush);
            vector<Brush*> brushArray;
            brushArray.push_back(&brush);
            if (m_postNotifications) brushesWillChange(brushArray);
            MoveResult result = brush.moveVertex(vertexIndex, delta);
            if (m_postNotifications) brushesWillChange(brushArray);
            return result;
        }

        MoveResult Map::moveEdge(Brush& brush, int edgeIndex, const Vec3f& delta) {
            if (find(m_selection->brushes().begin(), m_selection->brushes().end(), &brush) == m_selection->brushes().end())
                m_selection->addBrush(brush);
            vector<Brush*> brushArray;
            brushArray.push_back(&brush);
            if (m_postNotifications) brushesWillChange(brushArray);
            MoveResult result = brush.moveEdge(edgeIndex, delta);
            if (m_postNotifications) brushesWillChange(brushArray);
            return result;
        }

        MoveResult Map::moveFace(Brush& brush, int faceIndex, const Vec3f& delta) {
            if (find(m_selection->brushes().begin(), m_selection->brushes().end(), &brush) == m_selection->brushes().end())
                m_selection->addBrush(brush);
            vector<Brush*> brushArray;
            brushArray.push_back(&brush);
            if (m_postNotifications) brushesWillChange(brushArray);
            MoveResult result = brush.moveFace(faceIndex, delta);
            if (m_postNotifications) brushesDidChange(brushArray);
            return result;
        }

# pragma mark getters
        const BBox& Map::worldBounds() {
            return m_worldBounds;
        }

        Octree& Map::octree() {
            return *m_octree;
        }

        Picker& Map::picker() {
            return *m_picker;
        }

        Selection& Map::selection() {
            return *m_selection;
        }

        EntityDefinitionManager& Map::entityDefinitionManager() {
            return *m_entityDefinitionManager;
        }

        GroupManager& Map::groupManager() {
            return *m_groupManager;
        }

        const vector<string>& Map::mods() {
            return m_mods;
        }
    }
}