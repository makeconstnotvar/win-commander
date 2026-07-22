// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NCExplorerInspectorPlaceholderView.h"
#include <Utility/Layout.h>

@implementation NCExplorerInspectorPlaceholderView

- (instancetype)initWithFrame:(NSRect)frameRect
{
    self = [super initWithFrame:frameRect];
    if( self ) {
        NSTextField *label = [NSTextField
            labelWithString:NSLocalizedString(@"Select an item to see details",
                                               "Explorer mode inspector placeholder, shown when nothing is selected")];
        label.translatesAutoresizingMaskIntoConstraints = false;
        label.alignment = NSTextAlignmentCenter;
        label.textColor = NSColor.secondaryLabelColor;
        label.lineBreakMode = NSLineBreakByWordWrapping;
        [self addSubview:label];

        [self addConstraint:LayoutConstraintForCenteringViewHorizontally(label, self)];
        [self addConstraint:LayoutConstraintForCenteringViewVertically(label, self)];
        [self addConstraint:[NSLayoutConstraint constraintWithItem:label
                                                           attribute:NSLayoutAttributeWidth
                                                           relatedBy:NSLayoutRelationLessThanOrEqual
                                                              toItem:self
                                                           attribute:NSLayoutAttributeWidth
                                                          multiplier:0.8
                                                            constant:0.0]];
    }
    return self;
}

@end
