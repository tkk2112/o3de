/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */
//#include <API/EditorAssetSystemAPI.h>

#include <AzCore/StringFunc/StringFunc.h>
#include <AzCore/Utils/Utils.h>

#include <AzFramework/Asset/AssetSystemBus.h>
#include <AzFramework/Network/AssetProcessorConnection.h>
#include <AzFramework/StringFunc/StringFunc.h>
#include <AzToolsFramework/API/EditorAssetSystemAPI.h>
#include <AzToolsFramework/AssetBrowser/AssetBrowserBus.h>
#include <AzToolsFramework/AssetBrowser/Previewer/PreviewerBus.h>
#include <AzToolsFramework/AssetBrowser/Previewer/PreviewerFactory.h>
#include <AzToolsFramework/AssetBrowser/Entries/AssetBrowserEntry.h>
#include <AzToolsFramework/AssetBrowser/Entries/FolderAssetBrowserEntry.h>
#include <AzToolsFramework/AssetBrowser/Views/AssetBrowserTreeViewDialog.h>
#include <AzToolsFramework/AssetBrowser/Views/AssetBrowserViewUtils.h>
#include <AzToolsFramework/AssetBrowser/AssetSelectionModel.h>
#include <AzToolsFramework/Thumbnails/ThumbnailerBus.h>

#include <AzQtComponents/Components/Widgets/MessageBox.h>

#include <QDir>
#include <QPushButton>
#include <QSettings>
#include <QWidget>
#include <QtWidgets/QMessageBox>

namespace AzToolsFramework
{
    namespace AssetBrowser
    {
        bool AssetBrowserViewUtils::RenameEntry(const AZStd::vector<const AssetBrowserEntry*>& entries, QWidget* callingWidget)
        {
            if (entries.size() != 1)
            {
                return false;
            }
            using namespace AzFramework::AssetSystem;
            bool connectedToAssetProcessor = false;
            AzFramework::AssetSystemRequestBus::BroadcastResult(
                connectedToAssetProcessor, &AzFramework::AssetSystemRequestBus::Events::AssetProcessorIsReady);

            if (connectedToAssetProcessor)
            {
                using namespace AZ::IO;
                const AssetBrowserEntry* item = entries[0];
                bool isFolder = item->GetEntryType() == AssetBrowserEntry::AssetEntryType::Folder;
                Path toPath;
                Path fromPath;
                if (isFolder)
                {
                    // Cannot rename the engine or project folders
                    if (IsEngineOrProjectFolder(item->GetFullPath()))
                    {
                        return false;
                    }
                    // There is currently a bug in AssetProcessorBatch that doesn't handle empty folders
                    // This code is needed until that bug is fixed. GHI 13340
                    if (IsFolderEmpty(item->GetFullPath()))
                    {                           
                        return true;
                    }
                    fromPath = item->GetFullPath() + "/*";
                    toPath = item->GetFullPath() + "TempFolderTestName/*";
                }
                else
                {
                    fromPath = item->GetFullPath();
                    toPath = fromPath;
                    toPath.ReplaceExtension("renameFileTestExtension");
                }
                AssetChangeReportRequest request(
                    AZ::OSString(fromPath.c_str()), AZ::OSString(toPath.c_str()), AssetChangeReportRequest::ChangeType::CheckMove);
                AssetChangeReportResponse response;

                if (SendRequest(request, response))
                {
                    if (!response.m_lines.empty())
                    {
                        AZStd::string message;
                        AZ::StringFunc::Join(message, response.m_lines.begin(), response.m_lines.end(), "\n");
                        AzQtComponents::FixedWidthMessageBox msgBox(
                            600,
                            QObject::tr(isFolder ? "Before Rename Folder Information" : "Before Rename Asset Information"),
                            QObject::tr("The asset you are renaming may be referenced in other assets."),
                            QObject::tr("More information can be found by pressing \"Show Details...\"."),
                            message.c_str(),
                            QMessageBox::Warning,
                            QMessageBox::Cancel,
                            QMessageBox::Yes,
                            callingWidget);
                        auto* renameButton = msgBox.addButton(QObject::tr("Rename"), QMessageBox::YesRole);
                        msgBox.exec();

                        if (msgBox.clickedButton() == static_cast<QAbstractButton*>(renameButton))
                        {
                            return true;
                        }
                    }
                    else
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        void AssetBrowserViewUtils::AfterRename(QString newVal, const AZStd::vector<const AssetBrowserEntry*>& entries, QWidget* callingWidget)
        {
            if (entries.size() != 1)
            {
                return;
            }
            using namespace AZ::IO;
            const AssetBrowserEntry* item = entries[0];
            bool isFolder = item->GetEntryType() == AssetBrowserEntry::AssetEntryType::Folder;
            bool isEmptyFolder = isFolder && IsFolderEmpty(item->GetFullPath());
            Path toPath;
            Path fromPath;
            if (isFolder)
            {
                Path tempPath = item->GetFullPath();
                tempPath.ReplaceFilename(newVal.toStdString().c_str());
                // There is currently a bug in AssetProcessorBatch that doesn't handle empty folders
                // This code is needed until that bug is fixed. GHI 13340
                if (isEmptyFolder)
                {
                    fromPath = item->GetFullPath();
                    toPath = tempPath.String();
                    AZ::IO::SystemFile::Rename(fromPath.c_str(), toPath.c_str());
                    return;
                }
                else
                {
                    fromPath = item->GetFullPath() + "/*";
                    toPath = tempPath.String() + "/*";
                }
            }
            else
            {
                fromPath = item->GetFullPath();
                PathView extension = fromPath.Extension();
                toPath = fromPath;
                toPath.ReplaceFilename(newVal.toStdString().c_str());
                toPath.ReplaceExtension(extension);
            }
            // if the source path is the same as the destintion path then we don't need to go any further
            if (fromPath == toPath)
            {
                return;
            }
            using namespace AzFramework::AssetSystem;
            AssetChangeReportRequest moveRequest(
                AZ::OSString(fromPath.c_str()), AZ::OSString(toPath.c_str()), AssetChangeReportRequest::ChangeType::Move);
            AssetChangeReportResponse moveResponse;
            if (SendRequest(moveRequest, moveResponse))
            {
                // Send notification for listeners to update any in-memory states
                if (isFolder)
                {
                    AZStd::string_view fromPathNoWildcard(fromPath.ParentPath().Native());
                    AZStd::string_view toPathNoWildcard(toPath.ParentPath().Native());
                    AzToolsFramework::AssetBrowser::AssetBrowserFileActionNotificationBus::Broadcast(
                        &AzToolsFramework::AssetBrowser::AssetBrowserFileActionNotificationBus::Events::OnSourceFolderPathNameChanged,
                        fromPathNoWildcard,
                        toPathNoWildcard);
                }
                else
                {
                    AzToolsFramework::AssetBrowser::AssetBrowserFileActionNotificationBus::Broadcast(
                        &AzToolsFramework::AssetBrowser::AssetBrowserFileActionNotificationBus::Events::OnSourceFilePathNameChanged,
                        fromPath.c_str(),
                        toPath.c_str());
                }

                if (!moveResponse.m_lines.empty())
                {
                    AZStd::string message;
                    AZ::StringFunc::Join(message, moveResponse.m_lines.begin(), moveResponse.m_lines.end(), "\n");
                    AzQtComponents::FixedWidthMessageBox msgBox(
                        600,
                        QObject::tr(isFolder ? "After Rename Folder Information" : "After Rename Asset Information"),
                        QObject::tr("The asset has been renamed."),
                        QObject::tr("More information can be found by pressing \"Show Details...\"."),
                        message.c_str(),
                        QMessageBox::Information,
                        QMessageBox::Ok,
                        QMessageBox::Ok,
                        callingWidget);
                    msgBox.exec();
                }
                if (isFolder)
                {
                    AZ::IO::SystemFile::DeleteDir(item->GetFullPath().c_str());
                }
            }
        }

        bool AssetBrowserViewUtils::IsFolderEmpty(AZStd::string_view path)
        {
            return QDir(path.data()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty();
        }

        bool AssetBrowserViewUtils::IsEngineOrProjectFolder(AZStd::string_view path)
        {
            AZ::IO::PathView folderPathView(path);

            return (folderPathView.Compare(AZ::Utils::GetEnginePath()) == 0 || folderPathView.Compare(AZ::Utils::GetProjectPath()) == 0);
        }

        void AssetBrowserViewUtils::DeleteEntries(const AZStd::vector<const AssetBrowserEntry*>& entries, QWidget* callingWidget)
        {
            if (entries.empty())
            {
                return;
            }

            if (entries.size() > 1)
            {
                for (auto assetEntry : entries)
                {
                    if (assetEntry->GetEntryType() == AssetBrowserEntry::AssetEntryType::Folder)
                    {
                        return;
                    }
                }
            }
            bool isFolder = entries[0]->GetEntryType() == AssetBrowserEntry::AssetEntryType::Folder;
            if (isFolder && IsEngineOrProjectFolder(entries[0]->GetFullPath()))
            {
                return;
            }
            using namespace AzFramework::AssetSystem;
            bool connectedToAssetProcessor = false;
            AzFramework::AssetSystemRequestBus::BroadcastResult(
                connectedToAssetProcessor, &AzFramework::AssetSystemRequestBus::Events::AssetProcessorIsReady);

            if (connectedToAssetProcessor)
            {
                using namespace AZ::IO;
                for (auto item : entries)
                {
                    Path fromPath;
                    if (isFolder)
                    {
                        fromPath = item->GetFullPath() + "/*";
                    }
                    else
                    {
                        fromPath = item->GetFullPath();
                    }
                    AssetChangeReportRequest request(
                        AZ::OSString(fromPath.c_str()), AZ::OSString(""), AssetChangeReportRequest::ChangeType::CheckDelete, isFolder);
                    AssetChangeReportResponse response;

                    if (SendRequest(request, response))
                    {
                        bool canDelete = true;

                        if (!response.m_lines.empty())
                        {
                            AZStd::string message;
                            AZ::StringFunc::Join(message, response.m_lines.begin(), response.m_lines.end(), "\n");
                            AzQtComponents::FixedWidthMessageBox msgBox(
                                600,
                                QObject::tr(isFolder ? "Before Delete Folder Information" : "Before Delete Asset Information"),
                                QObject::tr("The asset you are deleting may be referenced in other assets."),
                                QObject::tr("More information can be found by pressing \"Show Details...\"."),
                                message.c_str(),
                                QMessageBox::Warning,
                                QMessageBox::Cancel,
                                QMessageBox::Yes,
                                callingWidget);
                            auto* deleteButton = msgBox.addButton(QObject::tr("Delete"), QMessageBox::YesRole);
                            msgBox.exec();

                            if (msgBox.clickedButton() != static_cast<QAbstractButton*>(deleteButton))
                            {
                                canDelete = false;
                            }
                        }
                        else
                        {
                            QMessageBox warningBox;
                            warningBox.setWindowTitle(QObject::tr(isFolder ? "Folder Deletion - Warning" : "Asset Deletion - Warning"));
                            warningBox.setText(
                                QObject::tr("O3DE is unable to detect if this file is being used inside a level, prefab, or asset."
                                            "By deleting these asset(s), you understand this might break a connection if it's still in use."));
                            warningBox.setInformativeText(
                                QObject::tr("Currently, delete is a permanent action and cannot be undone.\n"
                                    "Do you wish to proceed with this deletion?"));
                            warningBox.setIcon(QMessageBox::Warning);
                            warningBox.addButton(QMessageBox::Cancel);
                            warningBox.addButton(QObject::tr("Delete"), QMessageBox::YesRole);
                            warningBox.setDefaultButton(QMessageBox::Yes);
                            warningBox.setFixedWidth(600);
                            if (warningBox.exec() == QMessageBox::Cancel)
                            {
                                canDelete = false;
                            }
                        }

                        if (canDelete)
                        {
                            AssetChangeReportRequest deleteRequest(
                                AZ::OSString(fromPath.c_str()), AZ::OSString(""), AssetChangeReportRequest::ChangeType::Delete, isFolder);
                            AssetChangeReportResponse deleteResponse;
                            if (SendRequest(deleteRequest, deleteResponse))
                            {
                                if (!response.m_lines.empty())
                                {
                                    AZStd::string deleteMessage;
                                    AZ::StringFunc::Join(deleteMessage, response.m_lines.begin(), response.m_lines.end(), "\n");
                                    AzQtComponents::FixedWidthMessageBox deleteMsgBox(
                                        600,
                                        QObject::tr(isFolder ? "After Delete Folder Information" : "After Delete Asset Information"),
                                        QObject::tr("The asset has been deleted."),
                                        QObject::tr("More information can be found by pressing \"Show Details...\"."),
                                        deleteMessage.c_str(),
                                        QMessageBox::Information,
                                        QMessageBox::Ok,
                                        QMessageBox::Ok,
                                        callingWidget);
                                    deleteMsgBox.exec();
                                }
                            }
                            if (isFolder)
                            {
                                AZ::IO::SystemFile::DeleteDir(item->GetFullPath().c_str());
                            }
                        }
                    }
                }
            }
        }

        void AssetBrowserViewUtils::MoveEntries(const AZStd::vector<const AssetBrowserEntry*>& entries, QWidget* callingWidget)
        {
            if (entries.empty())
            {
                return;
            }

            if (entries.size() > 1)
            {
                for (auto assetEntry : entries)
                {
                    if (assetEntry->GetEntryType() == AssetBrowserEntry::AssetEntryType::Folder)
                    {
                        return;
                    }
                    
                }
            }
            bool isFolder = entries[0]->GetEntryType() == AssetBrowserEntry::AssetEntryType::Folder;
            if (isFolder && IsEngineOrProjectFolder(entries[0]->GetFullPath()))
            {
                return;
            }
            using namespace AzFramework::AssetSystem;
            EntryTypeFilter* foldersFilter = new EntryTypeFilter();
            foldersFilter->SetEntryType(AssetBrowserEntry::AssetEntryType::Folder);

            auto selection = AzToolsFramework::AssetBrowser::AssetSelectionModel::EverythingSelection();
            selection.SetTitle(QObject::tr("folder to move to"));
            selection.SetMultiselect(false);
            selection.SetDisplayFilter(FilterConstType(foldersFilter));
            AssetBrowserTreeViewDialog dialog(selection, callingWidget);

            if (dialog.exec() == QDialog::Accepted)
            {
                const AZStd::vector<AZStd::string> folderPaths = selection.GetSelectedFilePaths();

                if (!folderPaths.empty())
                {
                    AZStd::string folderPath = folderPaths[0];
                    bool connectedToAssetProcessor = false;
                    AzFramework::AssetSystemRequestBus::BroadcastResult(
                        connectedToAssetProcessor, &AzFramework::AssetSystemRequestBus::Events::AssetProcessorIsReady);

                    if (connectedToAssetProcessor)
                    {
                        for (auto entry : entries)
                        {
                            using namespace AZ::IO;
                            bool isEmptyFolder = isFolder && AssetBrowserViewUtils::IsFolderEmpty(entry->GetFullPath());
                            Path fromPath;
                            Path toPath;
                            if (isFolder)
                            {
                                Path filename = static_cast<Path>(entry->GetFullPath()).Filename();
                                if (isEmptyFolder)
                                // There is currently a bug in AssetProcessorBatch that doesn't handle empty folders
                                // This code is needed until that bug is fixed. GHI 13340
                                {
                                    fromPath = entry->GetFullPath();
                                    toPath =
                                        AZStd::string::format("%.*s/%.*s", AZ_STRING_ARG(folderPath), AZ_STRING_ARG(filename.Native()));
                                    AZ::IO::SystemFile::CreateDir(toPath.c_str());
                                    AZ::IO::SystemFile::DeleteDir(fromPath.c_str());
                                    return;
                                }
                                else
                                {
                                    fromPath = AZStd::string::format("%.*s/*", AZ_STRING_ARG(entry->GetFullPath()));
                                    toPath =
                                        AZStd::string::format("%.*s/%.*s/*", AZ_STRING_ARG(folderPath), AZ_STRING_ARG(filename.Native()));
                                }
                            }
                            else
                            {
                                fromPath = entry->GetFullPath();
                                PathView filename = fromPath.Filename();
                                toPath = folderPath;
                                toPath /= filename;
                            }
                            MoveEntry(fromPath.c_str(), toPath.c_str(), isFolder, callingWidget);
                        }
                    }
                }
            }
        }

        void AssetBrowserViewUtils::DuplicateEntries(const AZStd::vector<const AssetBrowserEntry*>& entries)
        {
            for (auto entry : entries)
            {
                using namespace AZ::IO;
                Path oldPath = entry->GetFullPath();
                AZ::IO::FixedMaxPath newPath =
                    AzFramework::StringFunc::Path::MakeUniqueFilenameWithSuffix(AZ::IO::PathView(oldPath.Native()), "-copy");
                QFile::copy(oldPath.c_str(), newPath.c_str());
            }
        }

        void AssetBrowserViewUtils::MoveEntry(AZStd::string_view fromPath, AZStd::string_view toPath, bool isFolder, QWidget* parent)
        {
            using namespace AzFramework::AssetSystem;
            AssetChangeReportRequest request(AZ::OSString(fromPath), AZ::OSString(toPath), AssetChangeReportRequest::ChangeType::CheckMove);
            AssetChangeReportResponse response;

            if (SendRequest(request, response))
            {
                bool canMove = true;

                if (!response.m_lines.empty())
                {
                    AZStd::string message;
                    AZ::StringFunc::Join(message, response.m_lines.begin(), response.m_lines.end(), "\n");
                    AzQtComponents::FixedWidthMessageBox msgBox(
                        600,
                        QObject::tr(isFolder ? "Before Move Folder Information" : "Before Move Asset Information"),
                        QObject::tr("The asset you are moving may be referenced in other assets."),
                        QObject::tr("More information can be found by pressing \"Show Details...\"."),
                        message.c_str(),
                        QMessageBox::Warning,
                        QMessageBox::Cancel,
                        QMessageBox::Yes,
                        parent);
                    auto* moveButton = msgBox.addButton(QObject::tr("Move"), QMessageBox::YesRole);
                    msgBox.exec();

                    if (msgBox.clickedButton() != static_cast<QAbstractButton*>(moveButton))
                    {
                        canMove = false;
                    }
                }
                if (canMove)
                {
                    AssetChangeReportRequest moveRequest(
                        AZ::OSString(fromPath), AZ::OSString(toPath), AssetChangeReportRequest::ChangeType::Move);
                    AssetChangeReportResponse moveResponse;
                    if (SendRequest(moveRequest, moveResponse))
                    {
                        // Send notification for listeners to update any in-memory states
                        if (isFolder)
                        {
                            // Remove the wildcard characters from the end of the paths
                            AZStd::string fromPathNoWildcard(fromPath.substr(0, fromPath.size() - 2));
                            AZStd::string toPathNoWildcard(toPath.substr(0, toPath.size() - 2));
                            AzToolsFramework::AssetBrowser::AssetBrowserFileActionNotificationBus::Broadcast(
                                &AzToolsFramework::AssetBrowser::AssetBrowserFileActionNotificationBus::Events::
                                    OnSourceFolderPathNameChanged,
                                fromPathNoWildcard.c_str(),
                                toPathNoWildcard.c_str());
                        }
                        else
                        {
                            AzToolsFramework::AssetBrowser::AssetBrowserFileActionNotificationBus::Broadcast(
                                &AzToolsFramework::AssetBrowser::AssetBrowserFileActionNotificationBus::Events::OnSourceFilePathNameChanged,
                                fromPath,
                                toPath);
                        }

                        if (!response.m_lines.empty())
                        {
                            AZStd::string moveMessage;
                            AZ::StringFunc::Join(moveMessage, response.m_lines.begin(), response.m_lines.end(), "\n");
                            AzQtComponents::FixedWidthMessageBox moveMsgBox(
                                600,
                                QObject::tr(isFolder ? "After Move Folder Information" : "After Move Asset Information"),
                                QObject::tr("The asset has been moved."),
                                QObject::tr("More information can be found by pressing \"Show Details...\"."),
                                moveMessage.c_str(),
                                QMessageBox::Information,
                                QMessageBox::Ok,
                                QMessageBox::Ok,
                                parent);
                            moveMsgBox.exec();
                        }
                    }
                    if (isFolder)
                    {
                        // Remove the wildcard characters from the end of the path
                        AZStd::string fromPathNoWildcard(fromPath.substr(0, fromPath.size() - 2));
                        if (!AZ::IO::SystemFile::DeleteDir(fromPathNoWildcard.c_str()))
                        {
                            AZ_Warning("AssetBrowser", false, "Failed to delete empty directory %s.", fromPathNoWildcard.c_str());
                        }
                    }
                }
            }
        }

        void AssetBrowserViewUtils::CopyEntry(AZStd::string_view fromPath, AZStd::string_view toPath, bool isFolder)
        {
            using namespace AZ::IO;
            Path oldPath = fromPath.data();
            Path newPath = toPath.data();

            if (oldPath == newPath)
            {
                newPath = AzFramework::StringFunc::Path::MakeUniqueFilenameWithSuffix(AZ::IO::PathView(oldPath.Native()), "-copy");
            }

            if (!isFolder)
            {
                QFile::copy(oldPath.c_str(), newPath.c_str());
            }

        }

        QVariant AssetBrowserViewUtils::GetThumbnail(const AssetBrowserEntry* entry, bool returnIcon)
        {
            // Check if this entry is a folder
            QString iconPathToUse;
            AZ::IO::FixedMaxPath engineRoot = AZ::Utils::GetEnginePath();
            AZ_Assert(!engineRoot.empty(), "Engine Root not initialized");
            if (auto folderEntry = azrtti_cast<const FolderAssetBrowserEntry*>(entry))
            {
                if (folderEntry->IsGemFolder())
                {
                    static constexpr const char* FolderIconPath = "Assets/Editor/Icons/AssetBrowser/GemFolder_80.svg";
                    iconPathToUse = (engineRoot / FolderIconPath).c_str();
                }
                else
                {
                    static constexpr const char* FolderIconPath = "Assets/Editor/Icons/AssetBrowser/Folder_80.svg";
                    iconPathToUse = (engineRoot / FolderIconPath).c_str();
                }
                return iconPathToUse;
            }

            // Check if this entry has a custom previewer, if so use that thumbnail
            if (!returnIcon)
            {
                AZ::EBusAggregateResults<const PreviewerFactory*> factories;
                PreviewerRequestBus::BroadcastResult(factories, &PreviewerRequests::GetPreviewerFactory, entry);
                for (const auto factory : factories.values)
                {
                    if (factory)
                    {
                        SharedThumbnail thumbnail;

                        ThumbnailerRequestBus::BroadcastResult(thumbnail, &ThumbnailerRequests::GetThumbnail, entry->GetThumbnailKey());
                        AZ_Assert(thumbnail, "The shared thumbnail was not available from the ThumbnailerRequestBus.");
                        if (thumbnail && thumbnail->GetState() != Thumbnail::State::Failed)
                        {
                            return thumbnail->GetPixmap();
                        }
                    }
                }
            }
            // Helper function to find the full path for a given icon
            auto findIconPath = [](QString resultPath)
            {
                // is it an embedded resource or absolute path?
                bool isUsablePath =
                    (resultPath.startsWith(":") || (!AzFramework::StringFunc::Path::IsRelative(resultPath.toUtf8().constData())));

                if (!isUsablePath)
                {
                    // getting here means it needs resolution.  Can we find the real path of the file?  This also searches in gems
                    // for sources.
                    bool foundIt = false;
                    AZStd::string watchFolder;
                    AZ::Data::AssetInfo assetInfo;
                    AssetSystemRequestBus::BroadcastResult(
                        foundIt,
                        &AssetSystemRequestBus::Events::GetSourceInfoBySourcePath,
                        resultPath.toUtf8().constData(),
                        assetInfo,
                        watchFolder);

                    if (foundIt)
                    {
                        // the absolute path is join(watchfolder, relativepath); // since its relative to the watch folder.
                        resultPath = QDir(watchFolder.c_str()).absoluteFilePath(assetInfo.m_relativePath.c_str());
                    }
                }
                return resultPath;
            };

            // Check if this is a product asset with an overridden icon
            if (auto productEntry = azrtti_cast<const ProductAssetBrowserEntry*>(entry))
            {
                AZ::AssetTypeInfoBus::EventResult(iconPathToUse, productEntry->GetAssetType(), &AZ::AssetTypeInfo::GetBrowserIcon);
                if (!iconPathToUse.isEmpty())
                {
                    return findIconPath(iconPathToUse);
                }
            }
            // Check if this asset has a custom icon
            AZ::EBusAggregateResults<SourceFileDetails> results;
            AssetBrowserInteractionNotificationBus::BroadcastResult(
                results, &AssetBrowserInteractionNotificationBus::Events::GetSourceFileDetails, entry->GetFullPath().c_str());

            auto it = AZStd::find_if(
                results.values.begin(),
                results.values.end(),
                [](const SourceFileDetails& details)
                {
                    return !details.m_sourceThumbnailPath.empty();
                });

            if (it != results.values.end())
            {
                iconPathToUse = findIconPath(QString::fromUtf8(it->m_sourceThumbnailPath.c_str()));
            }

            // No icon found - use default.
            if (iconPathToUse.isEmpty())
            {
                static constexpr const char* DefaultFileIconPath = "Assets/Editor/Icons/AssetBrowser/Default_16.svg";
                iconPathToUse = (engineRoot / DefaultFileIconPath).c_str();
            }
            return iconPathToUse;
        }

        Qt::DropAction AssetBrowserViewUtils::SelectDropActionForEntries(const AZStd::vector<const AssetBrowserEntry*>& entries)
        {
            QSettings settings;

            QVariant defaultDropMethod = settings.value("AssetBrowserDropAction", Qt::CopyAction);

            QString actionMessage = QObject::tr("Do you want to move or copy this asset?");
            if (entries.size() > 1)
            {
                actionMessage = QObject::tr("Do you want to move or copy these assets?");
            }

            QMessageBox messageBox;
            messageBox.setWindowTitle("Asset drop");
            messageBox.setText(actionMessage);
            messageBox.setIcon(QMessageBox::Question);
            messageBox.addButton(QMessageBox::Cancel);
            auto* moveButton = messageBox.addButton(QObject::tr("Move"), QMessageBox::YesRole);
            auto* copyButton = messageBox.addButton(QObject::tr("Copy"), QMessageBox::NoRole);
            messageBox.setDefaultButton(defaultDropMethod == Qt::MoveAction?moveButton:copyButton);
            messageBox.setFixedWidth(600);

            messageBox.exec();

            if (messageBox.clickedButton() == moveButton)
            {
                settings.setValue("AssetBrowserDropAction", Qt::MoveAction);
                return Qt::MoveAction;
            }
            else if (messageBox.clickedButton() == copyButton)
            {
                settings.setValue("AssetBrowserDropAction", Qt::CopyAction);
                return Qt::CopyAction;
            }
            else
            {
                return Qt::IgnoreAction;
            }
        }
    } // namespace AssetBrowser
} // namespace AzToolsFramework
