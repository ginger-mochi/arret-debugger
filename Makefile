CXX      ?= g++
BUILDDIR := build
CXXFLAGS := -Wall -Wextra -O2 -g -Iinclude -Ibackend -I$(BUILDDIR) -std=c++20
LDFLAGS  := -ldl -lm -lz

BACKEND_SRCS := $(wildcard backend/*.cpp backend/**/*.cpp)
HEADERS      := $(wildcard include/*.h) $(wildcard backend/*.hpp)

# Flatten backend source paths to build/<name>.o, prefixing subdir sources
#   backend/foo.cpp          -> build/foo.o
#   backend/arch/lr35902.cpp -> build/arch_lr35902.o
define backend_obj
$(BUILDDIR)/$(subst /,_,$(patsubst backend/%.cpp,%,$(1))).o
endef
BACKEND_OBJS := $(foreach s,$(BACKEND_SRCS),$(call backend_obj,$(s)))

ASSET_FILES  := $(wildcard assets/*)
BACKEND_OBJS += $(BUILDDIR)/assets.o

.PHONY: all clean sdl qt

all: sdl

sdl: arret-sdl
qt:  arret-qt

# ========== Assets (auto-generated from assets/) ==========

$(BUILDDIR)/assets.hpp: $(ASSET_FILES) Makefile | $(BUILDDIR)
	@printf '/* Generated from assets/ -- do not edit */\n#pragma once\n\nextern "C" {\n' > $@
	@for f in $(ASSET_FILES); do \
		sym=ar_asset_$$(basename "$$f" | tr '.-' '__'); \
		printf 'extern const unsigned char %s[];\n' "$$sym" >> $@; \
		printf 'extern const unsigned int  %s_size;\n' "$$sym" >> $@; \
	done
	@printf '}\n' >> $@

$(BUILDDIR)/assets.cpp: $(ASSET_FILES) Makefile | $(BUILDDIR)
	@printf '/* Generated from assets/ -- do not edit */\n' > $@
	@for f in $(ASSET_FILES); do \
		sym=ar_asset_$$(basename "$$f" | tr '.-' '__'); \
		printf '__asm__(".section .rodata,\\"a\\",@progbits\\n"\n' >> $@; \
		printf '    ".global %s\\n"\n' "$$sym" >> $@; \
		printf '    ".global %s_size\\n"\n' "$$sym" >> $@; \
		printf '    ".balign 16\\n"\n' >> $@; \
		printf '    "%s:\\n"\n' "$$sym" >> $@; \
		printf '    "  .incbin \\"%s\\"\\n"\n' "$$f" >> $@; \
		printf '    "%s_end:\\n"\n' "$$sym" >> $@; \
		printf '    ".balign 4\\n"\n' >> $@; \
		printf '    "%s_size:\\n"\n' "$$sym" >> $@; \
		printf '    "  .int %s_end - %s\\n"\n' "$$sym" "$$sym" >> $@; \
		printf '    ".previous\\n");\n\n' >> $@; \
	done

$(BUILDDIR)/assets.o: $(BUILDDIR)/assets.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Frontend objects that include assets.hpp
$(BUILDDIR)/sdl_main.o: $(BUILDDIR)/assets.hpp
$(BUILDDIR)/qt_main.o: $(BUILDDIR)/assets.hpp

# ========== Backend (shared) ==========

# Top-level backend sources
$(BUILDDIR)/%.o: backend/%.cpp $(HEADERS) | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Subdirectory backend sources (backend/dir/file.cpp -> build/dir_file.o)
$(BUILDDIR)/arch_%.o: backend/arch/%.cpp $(HEADERS) | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILDDIR)/gb_%.o: backend/gb/%.cpp $(HEADERS) | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILDDIR)/sys_%.o: backend/sys/%.cpp $(HEADERS) | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# ========== SDL frontend ==========

SDL_CFLAGS  := $(shell pkg-config --cflags sdl2)
SDL_LDFLAGS := $(shell pkg-config --libs sdl2)

$(BUILDDIR)/sdl_main.o: sdl/main.cpp $(HEADERS) | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(SDL_CFLAGS) -c -o $@ sdl/main.cpp

arret-sdl: $(BACKEND_OBJS) $(BUILDDIR)/sdl_main.o
	$(CXX) -o $@ $^ $(LDFLAGS) $(SDL_LDFLAGS)

# ========== Qt frontend ==========

QT_CFLAGS  := $(shell pkg-config --cflags Qt6Widgets Qt6Multimedia 2>/dev/null)
QT_LIBS    := $(shell pkg-config --libs Qt6Widgets Qt6Multimedia 2>/dev/null)
ifeq ($(QT_CFLAGS),)
  QT_INCDIR  ?= /usr/include/x86_64-linux-gnu/qt6
  QT_CFLAGS  := -I$(QT_INCDIR) -I$(QT_INCDIR)/QtCore -I$(QT_INCDIR)/QtGui \
                -I$(QT_INCDIR)/QtWidgets -I$(QT_INCDIR)/QtMultimedia
  QT_LIBS    := -lQt6Widgets -lQt6Gui -lQt6Core -lQt6Multimedia
endif
QT_CXXFLAGS := $(CXXFLAGS) -fPIC $(QT_CFLAGS)
MOC        ?= $(shell which moc6 2>/dev/null || echo /usr/lib/qt6/libexec/moc)

QT_SRCS     := $(wildcard qt/*.cpp)
QT_MOC_HDRS := $(wildcard qt/*.h)
QT_MOC_SRCS := $(patsubst qt/%.h,$(BUILDDIR)/moc_%.cpp,$(QT_MOC_HDRS))
QT_MOC_OBJS := $(QT_MOC_SRCS:.cpp=.o)
QT_OBJS     := $(patsubst qt/%.cpp,$(BUILDDIR)/qt_%.o,$(QT_SRCS))

# Qt subdirectory sources (qt/gb/*.cpp -> build/qt_gb_*.o)
QT_SUB_SRCS     := $(wildcard qt/*/*.cpp)
QT_SUB_MOC_HDRS := $(wildcard qt/*/*.h)
QT_SUB_MOC_SRCS := $(foreach h,$(QT_SUB_MOC_HDRS),$(BUILDDIR)/moc_$(subst /,_,$(patsubst qt/%.h,%,$(h))).cpp)
QT_SUB_MOC_OBJS := $(QT_SUB_MOC_SRCS:.cpp=.o)
QT_SUB_OBJS     := $(foreach s,$(QT_SUB_SRCS),$(BUILDDIR)/qt_$(subst /,_,$(patsubst qt/%.cpp,%,$(s))).o)

$(BUILDDIR)/moc_%.cpp: qt/%.h | $(BUILDDIR)
	$(MOC) $< -o $@

$(BUILDDIR)/moc_%.o: $(BUILDDIR)/moc_%.cpp | $(BUILDDIR)
	$(CXX) $(QT_CXXFLAGS) -c -o $@ $<

$(BUILDDIR)/qt_%.o: qt/%.cpp $(HEADERS) $(QT_MOC_HDRS) $(QT_SUB_MOC_HDRS) | $(BUILDDIR)
	$(CXX) $(QT_CXXFLAGS) -c -o $@ $<

# Qt subdir pattern rules
$(BUILDDIR)/moc_gb_%.cpp: qt/gb/%.h | $(BUILDDIR)
	$(MOC) $< -o $@

$(BUILDDIR)/qt_gb_%.o: qt/gb/%.cpp $(HEADERS) $(QT_MOC_HDRS) $(QT_SUB_MOC_HDRS) | $(BUILDDIR)
	$(CXX) $(QT_CXXFLAGS) -Iqt -c -o $@ $<

$(BUILDDIR)/moc_psx_%.cpp: qt/psx/%.h | $(BUILDDIR)
	$(MOC) $< -o $@

$(BUILDDIR)/qt_psx_%.o: qt/psx/%.cpp $(HEADERS) $(QT_MOC_HDRS) $(QT_SUB_MOC_HDRS) | $(BUILDDIR)
	$(CXX) $(QT_CXXFLAGS) -Iqt -c -o $@ $<

arret-qt: $(BACKEND_OBJS) $(QT_OBJS) $(QT_MOC_OBJS) $(QT_SUB_OBJS) $(QT_SUB_MOC_OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS) $(QT_LIBS)

# ========== Common ==========

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

clean:
	rm -rf $(BUILDDIR) arret-sdl arret-qt
