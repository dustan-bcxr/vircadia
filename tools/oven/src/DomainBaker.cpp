//
//  DomainBaker.cpp
//  tools/oven/src
//
//  Created by Stephen Birarda on 4/12/17.
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <QtCore/QDebug>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include "Gzip.h"

#include "DomainBaker.h"

DomainBaker::DomainBaker(const QUrl& localModelFileURL, const QString& domainName,
                         const QString& baseOutputPath, const QUrl& destinationPath) :
    _localEntitiesFileURL(localModelFileURL),
    _domainName(domainName),
    _baseOutputPath(baseOutputPath)
{
    // make sure the destination path has a trailing slash
    if (!destinationPath.toString().endsWith('/')) {
        _destinationPath = destinationPath.toString() + '/';
    } else {
        _destinationPath = destinationPath;
    }
}

void DomainBaker::start() {
    setupOutputFolder();
    loadLocalFile();
    enumerateEntities();
}

void DomainBaker::setupOutputFolder() {
    // in order to avoid overwriting previous bakes, we create a special output folder with the domain name and timestamp

    // first, construct the directory name
    auto domainPrefix = !_domainName.isEmpty() ? _domainName + "-" : "";
    auto timeNow = QDateTime::currentDateTime();

    static const QString FOLDER_TIMESTAMP_FORMAT = "yyyyMMdd-hhmmss";
    QString outputDirectoryName = domainPrefix + timeNow.toString(FOLDER_TIMESTAMP_FORMAT);

    //  make sure we can create that directory
    QDir outputDir { _baseOutputPath };

    if (!outputDir.mkpath(outputDirectoryName)) {

        // add an error to specify that the output directory could not be created

        return;
    }

    // store the unique output path so we can re-use it when saving baked models
    outputDir.cd(outputDirectoryName);
    _uniqueOutputPath = outputDir.absolutePath();

    // add a content folder inside the unique output folder
    static const QString CONTENT_OUTPUT_FOLDER_NAME = "content";
    if (!outputDir.mkpath(CONTENT_OUTPUT_FOLDER_NAME)) {
        // add an error to specify that the content output directory could not be created

        return;
    }

    _contentOutputPath = outputDir.absoluteFilePath(CONTENT_OUTPUT_FOLDER_NAME);
}

const QString ENTITIES_OBJECT_KEY = "Entities";

void DomainBaker::loadLocalFile() {
    // load up the local entities file
    QFile modelsFile { _localEntitiesFileURL.toLocalFile() };

    if (!modelsFile.open(QIODevice::ReadOnly)) {
        // add an error to our list to specify that the file could not be read

        // return to stop processing
        return;
    }

    // grab a byte array from the file
    auto fileContents = modelsFile.readAll();

    // check if we need to inflate a gzipped models file or if this was already decompressed
    static const QString GZIPPED_ENTITIES_FILE_SUFFIX = "gz";
    if (QFileInfo(_localEntitiesFileURL.toLocalFile()).suffix() == "gz") {
        // this was a gzipped models file that we need to decompress
        QByteArray uncompressedContents;
        gunzip(fileContents, uncompressedContents);
        fileContents = uncompressedContents;
    }

    // read the file contents to a JSON document
    auto jsonDocument = QJsonDocument::fromJson(fileContents);

    // grab the entities object from the root JSON object
    _entities = jsonDocument.object()[ENTITIES_OBJECT_KEY].toArray();

    if (_entities.isEmpty()) {
        // add an error to our list stating that the models file was empty

        // return to stop processing
        return;
    }
}

static const QString ENTITY_MODEL_URL_KEY = "modelURL";

void DomainBaker::enumerateEntities() {
    qDebug() << "Enumerating" << _entities.size() << "entities from domain";

    for (auto it = _entities.begin(); it != _entities.end(); ++it) {
        // make sure this is a JSON object
        if (it->isObject()) {
            auto entity = it->toObject();

            // check if this is an entity with a model URL
            if (entity.contains(ENTITY_MODEL_URL_KEY)) {
                // grab a QUrl for the model URL
                QUrl modelURL { entity[ENTITY_MODEL_URL_KEY].toString() };

                // check if the file pointed to by this URL is a bakeable model, by comparing extensions
                auto modelFileName = modelURL.fileName();

                static const QStringList BAKEABLE_MODEL_EXTENSIONS { ".fbx" };
                auto completeLowerExtension = modelFileName.mid(modelFileName.indexOf('.')).toLower();

                if (BAKEABLE_MODEL_EXTENSIONS.contains(completeLowerExtension)) {
                    // grab a clean version of the URL without a query or fragment
                    modelURL.setFragment(QString());
                    modelURL.setQuery(QString());

                    // setup an FBXBaker for this URL, as long as we don't already have one
                    if (!_bakers.contains(modelURL)) {
                        QSharedPointer<FBXBaker> baker { new FBXBaker(modelURL, _contentOutputPath) };

                        // make sure our handler is called when the baker is done
                        connect(baker.data(), &FBXBaker::finished, this, &DomainBaker::handleFinishedBaker);

                        // start the baker
                        baker->start();

                        // insert it into our bakers hash so we hold a strong pointer to it
                        _bakers.insert(modelURL, baker);
                    }

                    // add this QJsonValueRef to our multi hash so that we can easily re-write
                    // the model URL to the baked version once the baker is complete
                    _entitiesNeedingRewrite.insert(modelURL, *it);
                }
            }
        }
    }

    _enumeratedAllEntities = true;

    // check if it's time to write out the final entities file with re-written URLs
    possiblyOutputEntitiesFile();
}

void DomainBaker::handleFinishedBaker() {
    auto baker = qobject_cast<FBXBaker*>(sender());

    if (baker) {
        // this FBXBaker is done and everything went according to plan

        // enumerate the QJsonRef values for the URL of this FBX from our multi hash of
        // entity objects needing a URL re-write
        for (QJsonValueRef entityValue : _entitiesNeedingRewrite.values(baker->getFBXUrl())) {

            // convert the entity QJsonValueRef to a QJsonObject so we can modify its URL
            auto entity = entityValue.toObject();

            // grab the old URL
            QUrl oldModelURL { entity[ENTITY_MODEL_URL_KEY].toString() };

            // setup a new URL using the prefix we were passed
            QUrl newModelURL = _destinationPath.resolved(baker->getBakedFBXRelativePath().mid(1));

            // copy the fragment and query from the old model URL
            newModelURL.setQuery(oldModelURL.query());
            newModelURL.setFragment(oldModelURL.fragment());

            // set the new model URL as the value in our temp QJsonObject
            entity[ENTITY_MODEL_URL_KEY] = newModelURL.toString();

            // replace our temp object with the value referenced by our QJsonValueRef
            entityValue = entity;
        }

        // remove the baked URL from the multi hash of entities needing a re-write
        _entitiesNeedingRewrite.remove(baker->getFBXUrl());

        // check if it's time to write out the final entities file with re-written URLs
        possiblyOutputEntitiesFile();
    }
}

void DomainBaker::possiblyOutputEntitiesFile() {
    if (_enumeratedAllEntities && _entitiesNeedingRewrite.isEmpty()) {
        // we've enumerated all of our entities and re-written all the URLs we'll be able to re-write
        // time to write out a main models.json.gz file

        // first setup a document with the entities array below the entities key
        QJsonDocument entitiesDocument;

        QJsonObject rootObject;
        rootObject[ENTITIES_OBJECT_KEY] = _entities;

        entitiesDocument.setObject(rootObject);

        // turn that QJsonDocument into a byte array ready for compression
        QByteArray jsonByteArray = entitiesDocument.toJson();

        // compress the json byte array using gzip
        QByteArray compressedJson;
        gzip(jsonByteArray, compressedJson);

        // write the gzipped json to a new models file
        static const QString MODELS_FILE_NAME = "models.json.gz";

        auto bakedEntitiesFilePath = QDir(_uniqueOutputPath).filePath(MODELS_FILE_NAME);
        QFile compressedEntitiesFile { bakedEntitiesFilePath };

        if (!compressedEntitiesFile.open(QIODevice::WriteOnly)
            || (compressedEntitiesFile.write(compressedJson) == -1)) {
            qWarning() << "Failed to export baked entities file to" << bakedEntitiesFilePath;
            // add an error to our list to state that the output models file could not be created or could not be written to

            return;
        }

        qDebug() << "Exported entities file with baked model URLs to" << bakedEntitiesFilePath;

        // we've now written out our new models file - time to say that we are finished up
        emit finished();
    }
}

