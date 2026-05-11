CMAKE_MINIMUM_REQUIRED(VERSION 3.14)

set(QT5_MINIMUM_VERSION 5.15.2)
set(QT6_MINIMUM_VERSION 6.2.0)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOUIC_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/forms ${CMAKE_CURRENT_SOURCE_DIR}/settings/forms)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif ()

# This repo also supports autotools/depends builds which historically generate
# Qt MOC outputs in-tree (e.g. `src/qt/*.moc`, `src/qt/moc_*.cpp`). Those files
# are build artifacts and can conflict with CMake/Qt's AUTOMOC when switching
# between build systems (or Qt versions). Remove them to keep builds isolated.
file(GLOB _LEGACY_QT_MOC_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.moc"
    "${CMAKE_CURRENT_SOURCE_DIR}/moc_*.cpp"
)
if(_LEGACY_QT_MOC_FILES)
    file(REMOVE ${_LEGACY_QT_MOC_FILES})
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Gui Widgets Network LinguistTools Svg)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets Network LinguistTools Svg)
if (QT_VERSION_MAJOR EQUAL 6)
    find_package(Qt6 OPTIONAL_COMPONENTS DBus Charts OpenGL OpenGLWidgets)
else()
    find_package(Qt5 OPTIONAL_COMPONENTS DBus Charts OpenGL)
endif()

if (QT_VERSION_MAJOR EQUAL 6)
    if (Qt6Core_VERSION VERSION_LESS QT6_MINIMUM_VERSION)
        MESSAGE(FATAL_ERROR "Minimum supported Qt6 version is ${QT6_MINIMUM_VERSION}")
    else()
        MESSAGE(STATUS "Found Qt6 version: ${Qt6Core_VERSION}")
    endif()
elseif(QT_VERSION_MAJOR EQUAL 5)
    if (Qt5Core_VERSION VERSION_LESS QT5_MINIMUM_VERSION)
        MESSAGE(FATAL_ERROR "Minimum supported Qt5 version is ${QT5_MINIMUM_VERSION}")
    else()
        MESSAGE(STATUS "Found Qt5 version: ${Qt5Core_VERSION}")
    endif()
else()
    MESSAGE(FATAL_ERROR "Unsupported Qt major version: ${QT_VERSION_MAJOR}")
endif()

if (Qt${QT_VERSION_MAJOR}DBus_FOUND)
    add_compile_definitions(USE_DBUS)
endif()

if (Qt${QT_VERSION_MAJOR}Charts_FOUND)
    add_compile_definitions(USE_QTCHARTS)
endif()

if (QT_VERSION_MAJOR EQUAL 6 AND Qt6OpenGL_FOUND AND Qt6OpenGLWidgets_FOUND)
    add_compile_definitions(USE_GPU_TOPBAR_OVERLAY)
endif()

find_package(Qrcode)
if (QRCODE_FOUND)
    include_directories ( ${QRCODE_INCLUDE_DIR} )
    link_directories ( ${QRCODE_LIBRARY_DIRS} )
endif()

add_compile_options("-DQT_NO_KEYWORDS")

# Why isn't this done automatically??
if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    set(CMAKE_AUTOMOC_MOC_OPTIONS "-DQ_OS_MAC")
endif()

file(GLOB LOCAL_QT_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
source_group("QtHeaders" FILES ${LOCAL_QT_HEADERS})

SET(QT_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/bantablemodel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/bitcoinaddressvalidator.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/bitcoinunits.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/clientmodel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/csvmodelwriter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/guiutil.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/intro.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/networkstyle.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/notificator.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/optionsmodel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/peertablemodel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/qvalidatedlineedit.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/qvaluecombobox.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpcconsole.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpcexecutor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/trafficgraphwidget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/utilitydialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/addresstablemodel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/askpassphrasedialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/coincontroldialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/coincontroltreewidget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/editaddressdialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/openuridialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/paymentserver.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/transactionfilterproxy.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/transactionrecord.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/transactiontablemodel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/walletmodel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/walletmodeltransaction.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/pivxgui.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/loadingdialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/pwidget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/focuseddialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/guitransactionsutils.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/snackbar.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/navmenuwidget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/lockunlock.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/receivedialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/furabstractlistitemdelegate.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/txviewholder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/qtutils.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/balancebubble.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/expandablebutton.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/topbar.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/txrow.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/addressfilterproxymodel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/dashboardwidget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/addresslabelrow.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/contactdropdownrow.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/contactsdropdown.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/optionbutton.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/mninfodialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/coldstakingmodel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/mnmodel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/masternodewizarddialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/mnrow.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/masternodeswidget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/sendchangeaddressdialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/sendconfirmdialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/sendcustomfeedialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/sendmemodialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/sendmultirow.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/send.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/addnewcontactdialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/requestdialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/receivewidget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/myaddressrow.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/csrow.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/addressholder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tooltipmenu.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/addresseswidget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/defaultdialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/coldstakingwidget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/mnselectiondialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/proposalcard.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/votedialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/createproposaldialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/proposalinfodialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/governancewidget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/settings/backupwallet.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/settings/bittoolwidget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/settings/consolewidget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/settings/displayoptionswidget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/settings/faqwidget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/settings/informationwidget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/settings/mainoptionswidget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/settings/signmessagewidgets.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/settings/walletoptionswidget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/settings/walletrepairwidget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/settings/exportcsv.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/settings/settingswidget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/welcomecontentwidget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/splash.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/governancemodel.cpp
        )

set(PFBORDERIMAGE_MOC_CPP "${CMAKE_CURRENT_BINARY_DIR}/moc_pfborderimage.cpp")
if (QT_VERSION_MAJOR EQUAL 6)
    qt_generate_moc(pfborderimage.h ${PFBORDERIMAGE_MOC_CPP})
else()
    qt5_generate_moc(pfborderimage.h ${PFBORDERIMAGE_MOC_CPP})
endif()
list(APPEND QT_SOURCES ${PFBORDERIMAGE_MOC_CPP})

if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    list(APPEND QT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/macdockiconhandler.mm)
    list(APPEND QT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/macnotificationhandler.mm)
endif()

add_library(qt_stuff STATIC ${BitcoinHeaders} ${QtHeaders} ${QT_SOURCES} ${PROTO_SRCS} ${PROTO_HDRS})
target_link_libraries(qt_stuff PUBLIC Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::Svg)
if (Qt${QT_VERSION_MAJOR}DBus_FOUND)
    target_link_libraries(qt_stuff PUBLIC Qt${QT_VERSION_MAJOR}::DBus)
endif()
if (Qt${QT_VERSION_MAJOR}Charts_FOUND)
    target_link_libraries(qt_stuff PUBLIC Qt${QT_VERSION_MAJOR}::Charts)
endif()
if (QT_VERSION_MAJOR EQUAL 6 AND Qt6OpenGL_FOUND AND Qt6OpenGLWidgets_FOUND)
    target_link_libraries(qt_stuff PUBLIC Qt6::OpenGL Qt6::OpenGLWidgets)
endif()
target_include_directories(qt_stuff PUBLIC ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/leveldb/include
        ${CMAKE_SOURCE_DIR}/src/univalue/include
        ${CMAKE_SOURCE_DIR}/src/secp256k1/include
        ${CMAKE_SOURCE_DIR}/src/chiabls/src
        ${relic_SOURCE_DIR}/include
        ${relic_BINARY_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/settings
        ${BerkeleyDB_INCLUDE_DIRS}
        ${Boost_INCLUDE_DIRS}
        )
if (QT_VERSION_MAJOR EQUAL 6)
    set_property(TARGET qt_stuff PROPERTY CXX_STANDARD 17)
else()
    set_property(TARGET qt_stuff PROPERTY CXX_STANDARD 14)
endif()

file(GLOB QT_TRANSLATIONS_FILES ${CMAKE_CURRENT_SOURCE_DIR}/locale/*.ts)

get_filename_component(QM_FILES_BINARY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/locale" ABSOLUTE)
set_source_files_properties(${QT_TRANSLATIONS_FILES} PROPERTIES OUTPUT_LOCATION "${QM_FILES_BINARY_DIR}")

if (QT_VERSION_MAJOR EQUAL 6)
    qt_add_translation(QM ${QT_TRANSLATIONS_FILES})
else()
    qt5_add_translation(QM ${QT_TRANSLATIONS_FILES})
endif()
add_custom_target(translations_target ALL DEPENDS ${QM})

if (QT_VERSION_MAJOR EQUAL 6)
    qt_add_resources(QRC_RESOURCE pivx.qrc)
    qt_add_resources(QRC_LOCALE_RESOURCE pivx_locale.qrc)
else()
    qt5_add_resources(QRC_RESOURCE pivx.qrc)
    qt5_add_resources(QRC_LOCALE_RESOURCE pivx_locale.qrc)
endif()

add_executable(cteam-qt pivx.cpp ${QM} ${QRC_RESOURCE} ${QRC_LOCALE_RESOURCE})
add_dependencies(cteam-qt translations_target libunivalue libsecp256k1 libzcashrust leveldb crc32c bls)
target_include_directories(cteam-qt PUBLIC ${ENDIAN_INCLUDES} ${GMP_INCLUDE_DIR} ${Boost_INCLUDE_DIRS} ${sodium_INCLUDE_DIR})
target_link_libraries(cteam-qt
        qt_stuff
        univalue
        SERVER_A WALLET_A COMMON_A ZEROCOIN_A UTIL_A SAPLING_A BITCOIN_CRYPTO_A CLI_A
        leveldb crc32c secp256k1 rustzcash bls
        ${BerkeleyDB_LIBRARIES} ${Boost_LIBRARIES} ${LIBEVENT_LIB}
        ${sodium_LIBRARY_RELEASE} ${GMP_LIBRARY}
        -ldl pthread
        )
if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    target_link_libraries(cteam-qt "-framework Cocoa")
endif()
if(ZMQ_FOUND)
    target_link_libraries(cteam-qt ZMQ_A ${ZMQ_LIB})
    target_include_directories(cteam-qt PUBLIC ${ZMQ_INCLUDE_DIR})
endif()
if (QRCODE_FOUND)
    target_link_libraries(cteam-qt ${QRCODE_LIB})
    target_include_directories(cteam-qt PUBLIC ${QRCODE_INCLUDE_DIR})
endif()
if(MINIUPNP_FOUND)
    target_compile_definitions(cteam-qt PUBLIC "-DSTATICLIB -DMINIUPNP_STATICLIB")
    target_link_libraries(cteam-qt ${MINIUPNP_LIBRARY})
    target_include_directories(cteam-qt PUBLIC ${MINIUPNP_INCLUDE_DIR})
endif()
if(NAT-PMP_FOUND)
    target_link_libraries(cteam-qt ${NAT-PMP_LIBRARY})
    target_include_directories(cteam-qt PUBLIC ${NAT-PMP_INCLUDE_DIR})
endif()

	# Qt link dependencies are carried via qt_stuff.
